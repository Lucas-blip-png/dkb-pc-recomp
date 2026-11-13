"""Build/load a handwritten MSBuild task, then immediately clean its owned tree.

Run with Python 3.12 and --dotnet /path/to/dotnet (a .NET 10 SDK). No translator, game, GPU or NuGet download.
This is separate from the dependency-free installer tests because it exercises the actual SDK worker lifetime.
"""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from installer import bananza as b
from installer.events import Reporter

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dotnet', required=True, type=Path, help='dotnet executable with a .NET 10 SDK')
dotnet = parser.parse_args().dotnet.resolve()
with tempfile.TemporaryDirectory(prefix='installer-msbuild-') as temporary:
    base = Path(temporary)
    tools, workspace, dest = base/'tools', base/'owned', base/'installed'
    tools.mkdir()
    workspace.mkdir()
    owner = {'game':b.GAME, 'destination':str(dest), 'temporary':str(workspace)}
    (workspace/b.OWNER).write_text(json.dumps(owner))
    # Load a tiny task DLL in a parallel worker, the lifetime that retained ILCompiler.Build.Tasks.dll.
    # Restore uses no package feeds or external references.
    (tools/'build_xlate.py').write_text(r'''
import os, pathlib, subprocess, sys
from xml.sax.saxutils import escape
a=sys.argv; out=pathlib.Path(a[a.index('--out')+1]); out.mkdir(parents=True)
dotnet=pathlib.Path(os.environ['DOTNET_ROOT'])/('dotnet.exe' if os.name=='nt' else 'dotnet')
(out/'NuGet.Config').write_text('<configuration><packageSources><clear /></packageSources></configuration>')
(out/'Task.cs').write_text("""
using Microsoft.Build.Framework;
public sealed class CleanupTask : ITask {
    public IBuildEngine BuildEngine { get; set; }
    public ITaskHost HostObject { get; set; }
    public string Marker { get; set; }
    public bool Execute() {
        System.IO.File.WriteAllText(Marker, System.Environment.ProcessId.ToString());
        return true;
    }
}
""")
(out/'Task.csproj').write_text("""<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><TargetFramework>net10.0</TargetFramework><EnableDefaultCompileItems>false</EnableDefaultCompileItems></PropertyGroup>
  <ItemGroup><Compile Include="Task.cs" /><Reference Include="Microsoft.Build.Framework"><HintPath>$(MSBuildBinPath)/Microsoft.Build.Framework.dll</HintPath><Private>false</Private></Reference></ItemGroup>
</Project>""")
subprocess.run([dotnet,'build',out/'Task.csproj','--configfile',out/'NuGet.Config','-o',out/'task','-m:2','-nologo','-v:q'],check=True)
for index in range(2):
    assembly, marker = escape(str(out/'task/Task.dll')), escape(str(out/f'worker{index}.pid'))
    (out/f'worker{index}.proj').write_text(f'<Project><UsingTask TaskName="CleanupTask" AssemblyFile="{assembly}" /><Target Name="Run"><CleanupTask Marker="{marker}" /></Target></Project>')
(out/'parallel.proj').write_text('<Project><ItemGroup><Worker Include="worker0.proj;worker1.proj" /></ItemGroup><Target Name="Run"><MSBuild Projects="@(Worker)" Targets="Run" BuildInParallel="true" /></Target></Project>')
subprocess.run([dotnet,'msbuild',out/'parallel.proj','-t:Run','-m:2','-nologo','-v:q'],check=True)
(out/('totk_xlate.exe' if os.name=='nt' else 'totk_xlate')).write_bytes(b'synthetic translator')
(out/'totk_xlate.id').write_text('local-msbuild-probe\n')
''')
    ctx = SimpleNamespace(temp=workspace, dest=dest, layout=SimpleNamespace(root=base, tools=tools,
                           python=Path(sys.executable), dlls=(), llvm=dotnet.parent, sysroot=None),
                           options=SimpleNamespace(jobs=2),
                           reporter=Reporter(), info={'translator':None, 'identity':'local-msbuild-probe', 'game':b.GAME,
                                                     'dotnet':dotnet, 'owner':owner})
    before = dict(os.environ)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            b.Translator().run(ctx)
        workers = [int(p.read_text()) for p in (workspace/'xlate').glob('worker*.pid')]
        task = workspace/'xlate/task/Task.dll'
        assert task.is_file() and len(workers) == 2
        # No retry, chmod, process shutdown or global command: deletion must work on return.
        b.Cleanup().run(ctx)
        assert not task.exists() and not (workspace/'xlate').exists()
        assert dict(os.environ) == before
        print(json.dumps({'result':'pass', 'worker_pids':workers, 'cleanup':'immediate, no retries',
                          'sdk':subprocess.check_output([dotnet,'--version'],text=True).strip()}))
    except BaseException:
        for log in (workspace/'logs').glob('*.log'):
            print(log.read_text(encoding='utf-8', errors='replace'))
        raise
