/* V26 guard before the captured v27 parentless-entry regression fix. */
static int ui_modal_fits_v26(uint32_t manager, uint32_t window)
{
    if (!manager || manager != s2_touch_owner_manager() || !void_vm_readable(manager, 0x48) ||
        !window || !void_vm_readable(window, 0xEC) || MEM32(window + 4) != manager) return 0;
    uint32_t root = MEM32(manager + 0x44);
    if (!root || root == window || !void_vm_readable(root, 0xEC) || MEM32(root + 4) != manager ||
        MEM32(root + 0x58) || MEM32(root + 0x24) || MEM32(root + 0x28)) return 0;
    int left = (int)MEM32(window + 0x24), top = (int)MEM32(window + 0x28);
    int right = (int)MEM32(window + 0x2C), bottom = (int)MEM32(window + 0x30);
    int width, height, current_width, current_height;
    s2_game_resolution(&width, &height);
    void_input_screen_size(&current_width, &current_height);
    if (left < 0 || top < 0 || right <= left || bottom <= top ||
        width <= 0 || height <= 0 || current_width <= 0 || current_height <= 0 ||
        right > width || bottom > height || right > current_width || bottom > current_height ||
        right > (int)MEM32(root + 0x2C) || bottom > (int)MEM32(root + 0x30)) return 0;
    for (unsigned depth = 0; window && depth < 16 && void_vm_readable(window, 0x5C); ++depth) {
        if (window == root) return 1;
        window = MEM32(window + 0x58);
    }
    return 0;
}
