// The remapping rows of the menu's Controls tab (ui/binds_page.cpp). Present path only (ImGui).
#pragma once

// Draw the active device's bindings and the shared reset inside opt_begin / opt_end. After opt_end, binds_after_page
// shows the window that waits for a new binding.
void binds_rows(float scale, bool controller);
void binds_after_page(void);
// A cell waits for a press (or its press is still reaching the menu): B, Esc, L / R and the tab keys stay on the page.
bool binds_modal(void);
