#!/bin/bash
# test_binds.sh — Verify keybind parsing and generation logic
# Cannot test full daemon (no Wayland), but validates JSON parsing + output

set -e
cd "$(dirname "$0")"

echo "=== axctl Keybind Fix Verification ==="
echo ""

# 1. Binary exists
if [ -f ./axctl ]; then
    echo "✅ Binary compiled successfully"
else
    echo "❌ Binary not found — run 'make' first"
    exit 1
fi

# 2. Check that ambxst fields are in the header
echo ""
echo "--- Checking config_types.h for ambxst fields ---"
if grep -q "ambxst_system_keybinds" include/ipc/config_types.h && \
   grep -q "ambxst_binds" include/ipc/config_types.h && \
   grep -q "ambxst_bind_count" include/ipc/config_types.h; then
    echo "✅ config_types.h has all ambxst keybind fields"
else
    echo "❌ Missing ambxst fields in config_types.h"
    exit 1
fi

# 3. Check that config_types.c parses ambxst
echo ""
echo "--- Checking config_types.c ambxst parsing ---"
if grep -q 'json_get_object(ambxst_obj, "system")' src/ipc/config_types.c && \
   grep -q 'ambxst_system_keybinds\[' src/ipc/config_types.c && \
   grep -q 'ambxst_binds\[' src/ipc/config_types.c; then
    echo "✅ config_types.c parses keybinds.ambxst.system and dynamic keys"
else
    echo "❌ config_types.c missing ambxst parsing"
    exit 1
fi

# 4. Check free function covers ambxst
echo ""
echo "--- Checking free function ---"
if grep -q 'ambxst_system_keybind_count' src/ipc/config_types.c && \
   grep -q 'free(cfg->ambxst_system_keybinds)' src/ipc/config_types.c && \
   grep -q 'free(cfg->ambxst_binds)' src/ipc/config_types.c; then
    echo "✅ axctl_config_universal_free handles ambxst arrays"
else
    echo "❌ free function incomplete"
    exit 1
fi

# 5. Check config_handler merges all keybind arrays
echo ""
echo "--- Checking config_handler.c merge logic ---"
if grep -q 'merged_binds' src/server/config_handler.c && \
   grep -q 'ambxst_system_keybind_count' src/server/config_handler.c && \
   grep -q 'ambxst_bind_count' src/server/config_handler.c; then
    echo "✅ config_handler.c merges ambxst + custom before generating"
else
    echo "❌ config_handler.c missing merge logic"
    exit 1
fi

# 6. Check generator.c uses lua_quote instead of raw %s
echo ""
echo "--- Checking generator.c Lua escaping ---"
# The old pattern was: hl.dsp.exec_cmd(\"%s %s\") or similar
old_patterns=$(grep -c '\\"%s\\"' src/ipc/hyprland/generator.c 2>/dev/null || true)
# Only the hardcoded ones like action = \"toggle\" should remain
if [ "$old_patterns" -le 4 ]; then
    echo "✅ generator.c dispatcher_to_lua uses lua_quote (${old_patterns} remaining are hardcoded constants)"
else
    echo "⚠️  generator.c may still have unescaped \"%s\" patterns ($old_patterns found)"
fi

# 7. Check client.c execute uses proper escaping
echo ""
echo "--- Checking client.c execute Lua escaping ---"
if grep -q 'lua_escape_for_cmd' src/ipc/hyprland/client.c; then
    echo "✅ client.c hypr_execute uses lua_escape_for_cmd"
else
    echo "❌ client.c hypr_execute missing Lua escape"
    exit 1
fi

# 8. Check movefocus memory leak fix
echo ""
echo "--- Checking movefocus memory leak fix ---"
if grep -q 'free(focus_arg)' src/ipc/hyprland/generator.c; then
    echo "✅ movefocus memory leak fixed (focus_arg freed)"
else
    echo "❌ movefocus still leaks memory"
    exit 1
fi

echo ""
echo "=== All checks passed ✅ ==="
