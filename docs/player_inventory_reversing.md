# Player and Inventory Reversing Notes

This pass focuses on the player damage/contact path used by the new god-mode
hook. Inventory is partially anchored, but the runtime slot array is still a
TODO; do not treat the player contact arrays as inventory.

## Player Contact Impact

| Construct | Address | Notes |
| --- | ---: | --- |
| `Player_Update` | `0x0053C250` | Main Henry/player update. It reads the pending impact and branches into hit/knockback handlers when the value is greater than zero. |
| `Player_AccumulateContactImpact` | `0x0053F4E0` | Iterates contact entries and writes the summed impact to `0x010A3A04[player]`. |
| `Player_GetPendingImpact` | `0x0053F6D0` | Tiny getter returning `flt_10A3A00`; `Player_Update` consumes this value. |
| `Player_UpdateImpactTimer` | `0x0053F6E0` | Updates/returns the per-player recoil timer at `0x010A39C0[player]`. |
| Player block base | `0x010A0F20` | Player state blocks are addressed with a `0x420` stride. Henry is slot `0`. |
| Contact count | `0x010A3350` | Number of active contact entries. |
| Contact list | `0x010A33C0` | 48-byte entries; the float at `+0x04` contributes impact. |
| Pending impact | `0x010A3A00` | Global impact magnitude read by `Player_GetPendingImpact`. |
| Pending contact impact | `0x010A3A04` | Per-player accumulated contact impact. |
| Impact timer | `0x010A39C0` | Per-player hit/recoil timer. |
| Stance/action id | `0x010A39C8` | Returned by the observed stance accessor. |
| Facing radians | `0x010A39D0` | Used by knockback helpers with `sin`/`cos`. |

The god-mode hook detours `Player_AccumulateContactImpact`. When enabled for
Henry, it returns `0.0`, clears `0x010A3A00`, clears Henry's
`0x010A3A04[player]`, and clears Henry's recoil timer. It also attempts to hook
the tiny pending-impact getter; if MinHook cannot hook that short function, the
contact detour and EndScene safety clear still run.

Console:

```text
god
god on
god off
god status
```

This should cover normal enemy/contact damage and hit reactions. Scripted kill
volumes or bespoke event damage may still need separate reversing if they bypass
the contact-impact path.

## Inventory Anchors

The current confirmed inventory-related anchors are resource/UI names:

| Resource | Meaning |
| --- | --- |
| `item_model.bin` | Item model resource bundle. |
| `item_model2.bin` | Secondary item model resource bundle. |
| `item_l.bin` | Item UI/list resource bundle. |
| `message_item_msg_*.bin` | Localized item message resources. |

The runtime inventory construct is intentionally not represented as a concrete
slot struct yet. Earlier candidates around `0x010A33C0` and `0x010A3A20` are
player contact/hit-entry storage, not inventory. Future inventory work should
start from item UI/menu code and storage-box transitions, then add a concrete
layout only once reads/writes are confirmed in IDA and in game.
