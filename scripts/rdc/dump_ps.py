# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# qrenderdoc --python dump_ps.py (env RDC, OUT, EVENT): writes the draw's fragment shader SPIR-V to OUT.
import os
import renderdoc as rd
cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDC"], "", None)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
ctrl.SetFrameEvent(int(os.environ["EVENT"]), True)
refl = ctrl.GetPipelineState().GetShaderReflection(rd.ShaderStage.Fragment)
open(os.environ["OUT"], "wb").write(bytes(refl.rawBytes))
ctrl.Shutdown(); cap.Shutdown()
os._exit(0)
