# Silent Hill 4 Input Rebinding Notes

Reverse-engineered from the GOG `SILENT HILL 4.exe`.

## Keyboard path

`Input_CreateDevices` (`0x00406980`) creates three DirectInput devices:

- `inputManager+4`: `GUID_SysKeyboard`
- `inputManager+8`: `GUID_SysMouse`
- `inputManager+12`: enumerated gamepad

`Input_PollDevices` (`0x004074C0`) polls the keyboard with:

```c
keyboard->GetDeviceState(256, keyState);
```

It then walks all 256 DirectInput scan offsets and routes key transitions into
`Input_UpdateKeyboardButtonMasks` (`0x00406FF0`). The mouse path in the same
function is menu-cursor/buttons only; it does not feed stock camera look.

## Modern strafe remap

The proxy remaps keyboard state at the DirectInput boundary in
`src/hooks/dinput8_hook.cpp`:

- `DIK_A` (`0x1E`) is ORed into `DIK_Q` (`0x10`)
- `DIK_D` (`0x20`) is ORed into `DIK_E` (`0x12`)
- `DIK_A` and `DIK_D` are then cleared

That makes A/D trigger the game's existing Q/E strafe actions while suppressing
the original A/D turn actions. The hook also rewrites buffered keyboard events as
a fallback, but the stock GOG path observed here uses immediate keyboard state.
