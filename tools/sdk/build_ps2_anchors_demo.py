#!/usr/bin/env python3
"""Build a small PS2 anchor index from cached Ghidra decompiles (demo fixture).

The live Ghidra MCP server in this environment is not reachable over plain HTTP
(it is bridged via stdio), so `export_anchors.py --engine ghidra` can't be run
against it directly here. This fixture reproduces exactly what that exporter would
emit for a representative slice of the PS2 camera code: it feeds real Ghidra
pseudocode (game_camera_3ldk.c, from SLUS_208.73) through the *same*
`extract_anchors()` used for the PC side, so the two indexes are strictly
comparable.

To regenerate against a live Ghidra MCP HTTP endpoint instead:
    python tools/sdk/export_anchors.py --engine ghidra --url http://127.0.0.1:<port> \
        --filter cam --filter camera -o tools/sdk/generated/ps2_anchors.json
"""

from __future__ import annotations

from pathlib import Path

from autocorrelate import write_json
from export_anchors import extract_anchors

GEN = Path(__file__).resolve().parents[2] / "tools" / "sdk" / "generated"

# name -> (address, ghidra pseudocode). Addresses from the SLUS_208.73 symbol table.
FUNCS = {
    "cam3GetShakeHeight": ("0x1d21e0", r"""
float cam3GetShakeHeight(void)
{
  PlayerAnotherGetUI(0);
  fVar2 = player_ext_info[0].moved_speed[2];
  fVar1 = fabsf((float)player_ext_info[0].moved_speed._0_8_);
  fVar2 = fabsf(fVar2);
  if (fVar2 < fVar1) { fVar2 = fVar1; }
  fVar2 = fVar2 / 250.0;
  if (player_ext_info[0].status == PLAYER_STAT_MOVE) {
    if (fVar2 < 0.0) { fVar2 = fVar2 / 0.45; }
    else if (0.0 < fVar2) { fVar2 = fVar2 * 1.3; }
    tf_2428 = tf_2428 + (fVar2 * 3.1415927) / 15.0;
    if (3.1415927 < tf_2428) { tf_2428 = tf_2428 - 6.2831855; sfSeCallEx(0x9c51,1.0,0.0,0,1); }
    else if (tf_2428 < -3.1415927) { tf_2428 = tf_2428 + 6.2831855; sfSeCallEx(0x9c51,1.0,0.0,0,1); }
  }
  fVar2 = sinf(tf_2428);
  return fVar2;
}
"""),
    "cam3DecidePosition": ("0x1d1f00", r"""
void cam3DecidePosition(void)
{
  fStack_10 = *(float *)((int)gamew.player + 0x40);
  if (*(int *)((int)gamew.player + 0x224) < 100) {
    if (gamew.stage == 10) { fStack_c = *(float *)((int)gamew.player + 0x44) - 120.00001; }
    else { fStack_c = -300.0; }
  }
  else { fStack_c = *(float *)((int)gamew.player + 0x44) - 300.0; }
  if (((char)gamew.stage != 10) && (*(int *)((int)gamew.player + 0x224) < 100)) {
    fStack_c = -300.0;
    fVar2 = cam3GetShakeHeight();
    paVar1 = miscOptionGetPtr();
    if (paVar1->head_motion == 2) { fStack_c = fStack_c + fVar2 * 3.0; }
    else if (paVar1->head_motion == 1) { fStack_c = fStack_c + fVar2 * 0.5 * 3.0; }
  }
  cam_height = fStack_c;
  cam3Move(&fStack_10);
}
"""),
    "cam3InitState": ("0x1d1b90", r"""
void cam3InitState(void)
{
  cam_height = 0.0;
  cambw.run = 1;
  cambw.lookAtHeight = 209.99998;
  if (gamew.stage == 10) { cambw.lookAtHeight = 275.0; }
  cambw.fov = 0.9599311;
  sfCameraSetViewAngle(0.9599311);
  GameCameraViewAngleSet(cambw.fov);
  cam3_work.mtx[3][1] = *(float *)((int)pvVar1 + 0x44) - 300.0;
  camBaseMakeLookAt(&fStack_10,(float *)((int)pvVar1 + 0x60),cambw.focalLength);
  cam3RotFromTargetPos(&fStack_10);
  cam3Rotate(cam3_work.mtx,cam3_work.rot);
  cam3Move((float *)((int)pvVar1 + 0x40));
}
"""),
    "cam3RotFromTargetPos": ("0x1d1890", r"""
void cam3RotFromTargetPos(float *target)
{
  fVar1 = atan2f(uStack_10,uStack_8);
  fVar1 = sgMathAngleRegulateF(fVar1);
  fVar2 = sgMathAtan2F(uStack_c,SQRT(uStack_10 * uStack_10 + uStack_8 * uStack_8));
  cam3_work.rot[0] = fVar2 * -1.0;
  cam3_work.rot[1] = fVar1;
  cam3_work.rot[2] = 0.0;
}
"""),
    "Camera3ldkMain": ("0x1d16b0", r"""
void Camera3ldkMain(void)
{
  pvVar4 = gamew.player;
  if ((((cambw.run == 1) && (iVar5 = PlayerUIMotion(), iVar5 == 0)) ||
      (iVar5 = Player3ldkIsForceRotate(), iVar5 == 1)) && (*(int *)((int)pvVar4 + 0x224) < 100)) {
    cam3InitRuntime();
    if (cam3_work.step == CAM3_STEP_AUTOFOCUS) { cam3AFLookAtItem(); cam3Rotate(cam3_work.mtx,&fStack_10); }
    else if (cam3_work.step == CAM3_STEP_NORMAL) { cam3Rotate(cam3_work.mtx,&fStack_10); }
    cam3DecidePosition();
    cam3FinalizeRuntime();
  }
  else if (99 < *(int *)((int)pvVar4 + 0x224)) {
    cam3Rotate(cam3_work.mtx,(float *)((int)pvVar4 + 0x60));
    cam3DecidePosition();
    cam3FinalizeRuntime();
  }
}
"""),
    "cam3SetRotFromZ": ("0x1d1a90", r"""
void cam3SetRotFromZ(void)
{
  pfVar2 = sfCameraDirZ();
  cam3_work.rot[0] = asinf((float)((ulong)uVar1 >> 0x20) * -1.0);
  cam3_work.rot[1] = atan2f((float)uVar1,__x);
  cam3_work.rot[2] = 0.0;
}
"""),
    "cam3InitRuntime": ("0x1d1e00", r"""
void cam3InitRuntime(void)
{
  cam3_afwork.cancel = 0;
  if (cam3_work.step == 0) { cam3SetRotFromZ(); }
  cam3_work.focalLength = 600.0;
}
"""),
}


def main() -> int:
    records = []
    for name, (addr, code) in FUNCS.items():
        strings, consts = extract_anchors(code)
        records.append({"addr": addr, "name": name, "strings": strings, "consts": consts})
    out = GEN / "ps2_anchors.json"
    write_json(out, {"engine": "ghidra", "binary": "SLUS_208.73", "functions": records})
    print(f"wrote {out}  ({len(records)} functions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
