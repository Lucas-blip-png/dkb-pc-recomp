// The light-quad fold (spirv_lq_fold.h): word-level, no SPIR-V library. A vertex module is read once for the facts the
// pattern needs, the `& 3` results are classified by their uses, and the chosen ones become OpCopyObject of a null
// constant of their type. Nothing else in the module changes, so the driver compiles the same code with r fixed at 0.
#include "spirv_lq_fold.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace {

enum : uint32_t {
    OpEntryPoint = 15, OpTypeInt = 21, OpConstant = 43, OpConstantNull = 46, OpFunction = 54, OpFunctionEnd = 56,
    OpVariable = 59, OpLoad = 61, OpFunctionCall = 57, OpLabel = 248, StorageFunction = 7, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpStore = 62, OpCopyObject = 83,
    OpBitcast = 124, OpSelect = 169, OpShiftRightArithmetic = 195, OpBitwiseAnd = 199, OpSelectionMerge = 247,
    OpBranchConditional = 250, OpSwitch = 251,
    DecorationLocation = 30, StorageInput = 1, StorageUniform = 2, StorageOutput = 3, StorageStorageBuffer = 12,
    ModelVertex = 0, ModelFragment = 4,
};

// Opcodes whose first operand is the result type and second the result id (enough of them to follow data flow).
bool type_first(uint32_t op) {
    return op == 12 || (op >= 41 && op <= 46) || (op >= 48 && op <= 52) || op == 54 || op == 55 || op == 57 || op == 59 ||
           op == 60 || op == 61 || (op >= 65 && op <= 70) || (op >= 77 && op <= 84) || (op >= 86 && op <= 107) ||
           (op >= 109 && op <= 212) || (op >= 227 && op <= 242) || op == 245;
}

// An `x & 3` (one operand the integer constant 3) and what its result is used for.
struct And3 {
    uint32_t other = 0, type = 0;
    bool comp = false, test = false;   // comp: the last index of a chain into a buffer; test: a compare, select or switch
};

}  // namespace

extern "C" int spirv_lq_fold(const uint32_t *w, uint64_t n64, int construct, uint32_t **out, uint64_t *out_n, unsigned *nulled) {
    if (out) *out = nullptr;
    if (nulled) *nulled = 0;
    const size_t n = static_cast<size_t>(n64);
    if (!w || n < 5 || w[0] != 0x07230203u) return -1;
    for (size_t i = 5; i < n;) {   // every instruction must fit
        const uint32_t len = w[i] >> 16;
        if (!len || i + len > n) return -1;
        i += len;
    }

    // Module facts: entry point models, integer types and constants, variables, Location decorations, the `& 3`
    // results and the shifts that feed them, where the first function starts.
    bool vertex = false, fragment = false;
    std::set<uint32_t> int_types;
    std::map<uint32_t, uint32_t> iconst, sra_shift, loc_of;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> vars;   // id -> (pointer type, storage class)
    std::map<uint32_t, And3> and3;
    size_t first_fn = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        switch (op) {
        case OpEntryPoint:
            if (len >= 3) vertex |= a[0] == ModelVertex, fragment |= a[0] == ModelFragment;
            break;
        case OpTypeInt:
            if (len >= 4) int_types.insert(a[0]);
            break;
        case OpConstant:
            if (len >= 4 && int_types.count(a[0])) iconst[a[1]] = a[2];
            break;
        case OpVariable:
            if (len >= 4) vars[a[1]] = {a[0], a[2]};
            break;
        case OpDecorate:
            if (len >= 4 && a[1] == DecorationLocation) loc_of[a[0]] = a[2];
            break;
        case OpFunction:
            if (!first_fn) first_fn = i;
            break;
        case OpShiftRightArithmetic:
            if (len >= 5) sra_shift[a[1]] = a[3];
            break;
        case OpBitwiseAnd:
            if (len >= 5) {
                auto c2 = iconst.find(a[2]), c3 = iconst.find(a[3]);
                if (c3 != iconst.end() && c3->second == 3) and3[a[1]].other = a[2], and3[a[1]].type = a[0];
                else if (c2 != iconst.end() && c2->second == 3) and3[a[1]].other = a[3], and3[a[1]].type = a[0];
            }
            break;
        }
        i += len;
    }
    if (!vertex || fragment || !first_fn || and3.empty()) return 0;
    bool loc9 = false;   // an Input variable decorated Location 9: the ten per-instance streams of the light quads
    for (auto &v : vars) {
        auto l = loc_of.find(v.first);
        if (v.second.second == StorageInput && l != loc_of.end() && l->second == 9) loc9 = true;
    }
    if (construct < 0 && !loc9) return 0;

    // Classify each `& 3` by its uses, through bitcasts and copies. round: (x >> 31) & 3, the rounding fix-up of a signed
    // divide by 4; comp: the last index of an access chain into a uniform or storage buffer; test: an operand of an
    // integer compare, an OpSelect or the selector of an OpSwitch. In that priority; only the test class is folded.
    // A value stored to a function-local variable and loaded back in the same block is followed too, as SPIRV-Tools'
    // local single-block load/store elimination would forward it: the translator routes values through such locals, and
    // without the forwarding the compare reads a load, not the `& 3`.
    std::map<uint32_t, uint32_t> root_of, block_store;   // block_store: local variable -> the root last stored in this block
    for (auto &kv : and3) root_of[kv.first] = kv.first;
    bool in_fn = false;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpFunction) in_fn = true;
        if (op == OpFunctionEnd) in_fn = false;
        if (op == OpLabel || op == OpFunctionCall) block_store.clear();
        if (in_fn) {
            if ((op == OpBitcast || op == OpCopyObject) && len >= 4) {
                auto r = root_of.find(w[i + 3]);
                if (r != root_of.end()) root_of[w[i + 2]] = r->second;
            }
            if (op == OpStore && len >= 3) {
                auto v = vars.find(w[i + 1]);
                if (v != vars.end() && v->second.second == StorageFunction) {
                    auto r = root_of.find(w[i + 2]);
                    if (r != root_of.end()) block_store[w[i + 1]] = r->second;
                    else block_store.erase(w[i + 1]);
                }
            } else if (op == OpLoad && len >= 4) {
                auto s = block_store.find(w[i + 3]);
                if (s != block_store.end()) root_of[w[i + 2]] = s->second;
            }
            for (size_t k = 1; k < len; k++) {
                auto r = root_of.find(w[i + k]);
                if (r == root_of.end() || (k == 2 && type_first(op))) continue;
                And3 &x = and3[r->second];
                if ((op == OpAccessChain || op == OpInBoundsAccessChain) && k == len - 1u) {
                    auto bv = vars.find(w[i + 3]);
                    if (bv != vars.end() && (bv->second.second == StorageUniform || bv->second.second == StorageStorageBuffer)) x.comp = true;
                } else if ((op >= 170 && op <= 179) || op == OpSelect || (op == OpSwitch && k == 1)) {
                    x.test = true;
                }
            }
        }
        i += len;
    }
    std::set<uint32_t> tests;
    for (auto &kv : and3) {
        const And3 &x = kv.second;
        auto sh = sra_shift.find(x.other);
        auto sc = sh == sra_shift.end() ? iconst.end() : iconst.find(sh->second);
        const bool round = sc != iconst.end() && sc->second == 31;
        if (!round && !x.comp && x.test) tests.insert(kv.first);
    }

    std::set<uint32_t> fold;   // the `& 3` results that become 0
    if (construct < 0) {
        if (tests.size() != 2) return 0;
        fold = tests;
    } else {
        // Data flow (through arithmetic, selects, access chains and stores to function locals) from each test-class result
        // to the branch conditions it reaches; a condition right after OpSelectionMerge number k decides construct k.
        std::map<uint32_t, std::vector<uint32_t>> flow;
        std::map<uint32_t, uint32_t> chain_of;
        std::map<uint32_t, unsigned> construct_of;   // branch condition id -> the selection construct it decides
        unsigned sel_counter = 0, sel_cur = 0;
        bool sel_pending = false;
        in_fn = false;
        for (size_t i = 5; i < n;) {
            const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
            const uint32_t *a = w + i + 1;
            if (op == OpFunction) in_fn = true;
            if (op == OpFunctionEnd) in_fn = false;
            if (in_fn) {
                if (op == OpSelectionMerge) sel_cur = sel_counter++, sel_pending = true;
                else if (op != OpBranchConditional) sel_pending = false;
                auto base_of = [&](uint32_t id) { auto c = chain_of.find(id); return c == chain_of.end() ? id : c->second; };
                if (op == OpStore && len >= 3) {
                    const uint32_t base = base_of(a[0]);
                    auto v = vars.find(base);
                    if (v == vars.end() || v->second.second != StorageOutput) flow[a[1]].push_back(base);
                } else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 5) {
                    chain_of[a[1]] = base_of(a[2]);
                    flow[a[2]].push_back(a[1]);
                    for (size_t k = 3; k + 1 < len; k++) flow[a[k]].push_back(a[1]);
                } else if (op == OpBranchConditional || op == OpSwitch) {
                    if (sel_pending) construct_of[a[0]] = sel_cur;
                } else if (type_first(op) && len >= 4) {
                    for (size_t k = 2; k + 1 < len; k++) flow[a[k]].push_back(a[1]);
                }
            }
            i += len;
        }
        for (uint32_t t : tests) {
            std::set<uint32_t> seen;
            std::vector<uint32_t> work{t};
            bool decides = false;
            while (!work.empty() && !decides) {
                const uint32_t v = work.back();
                work.pop_back();
                if (!seen.insert(v).second) continue;
                auto c = construct_of.find(v);
                decides = c != construct_of.end() && c->second == static_cast<unsigned>(construct);
                auto f = flow.find(v);
                if (f != flow.end()) work.insert(work.end(), f->second.begin(), f->second.end());
            }
            if (decides) fold.insert(t);
        }
        if (fold.empty()) return 0;
    }
    if (nulled) *nulled = static_cast<unsigned>(fold.size());
    if (!out) return 1;

    // The copy: one OpConstantNull per result type of a folded `& 3`, placed before the first function, and each folded
    // OpBitwiseAnd replaced by an OpCopyObject of that null (same type, same result id, so every use stays valid).
    uint32_t bound = w[3];
    std::map<uint32_t, uint32_t> null_of;
    for (uint32_t id : fold) {
        const uint32_t t = and3[id].type;
        if (!null_of.count(t)) null_of[t] = bound++;
    }
    std::vector<uint32_t> o;
    o.reserve(n + 3 * null_of.size());
    o.insert(o.end(), w, w + 5);
    o[3] = bound;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_fn)
            for (auto &kv : null_of) o.insert(o.end(), {3u << 16 | OpConstantNull, kv.first, kv.second});
        if (op == OpBitwiseAnd && len >= 5 && fold.count(w[i + 2])) {
            o.insert(o.end(), {4u << 16 | OpCopyObject, w[i + 1], w[i + 2], null_of[w[i + 1]]});
        } else {
            o.insert(o.end(), w + i, w + i + len);
        }
        i += len;
    }
    uint32_t *copy = static_cast<uint32_t *>(std::malloc(o.size() * sizeof(uint32_t)));
    if (!copy) return -1;
    std::memcpy(copy, o.data(), o.size() * sizeof(uint32_t));
    *out = copy;
    *out_n = o.size();
    return 1;
}
