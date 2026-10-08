"""Export Depth Anything V2 Small for Steep's isolated native compositor tests."""
import argparse
import json
import sys
import hashlib
import subprocess
from pathlib import Path
import numpy as np
import torch
import onnxruntime as ort

p=argparse.ArgumentParser()
p.add_argument('--source',required=True)
p.add_argument('--checkpoint',required=True)
p.add_argument('--output',required=True)
a=p.parse_args()
revision = subprocess.check_output(['git', '-C', a.source, 'rev-parse', 'HEAD'], text=True).strip()
if revision != 'a561b849ebae10a6f5ef49e26c83cbbcd36c71bf':
    raise RuntimeError('Use the validated Depth Anything upstream revision from compositing-models.json')
if hashlib.sha256(Path(a.checkpoint).read_bytes()).hexdigest() != '715fade13be8f229f8a70cc02066f656f2423a59effd0579197bbf57860e1378':
    raise RuntimeError('The depth checkpoint does not match the validated SHA-256')
sys.path.insert(0,a.source)
from depth_anything_v2.dpt import DepthAnythingV2
torch.set_num_threads(4)
model=DepthAnythingV2(encoder='vits',features=64,out_channels=[48,96,192,384]).eval()
model.load_state_dict(torch.load(a.checkpoint,map_location='cpu',weights_only=True))
torch.manual_seed(123)
image=torch.randn(1,3,518,518)
with torch.inference_mode():
    expected=model(image).numpy()
    torch.onnx.export(model,(image,),a.output,input_names=['image'],output_names=['depth'],opset_version=17,dynamo=False)
    opts=ort.SessionOptions()
    opts.intra_op_num_threads=4
    session=ort.InferenceSession(a.output,opts,providers=['CPUExecutionProvider'])
    actual=session.run(None,{'image':image.numpy()})[0]
    error=float(np.max(np.abs(actual-expected)))
    relative=error/max(float(np.max(np.abs(expected))),1e-6)
    report={'max_absolute_error':error,'relative_max_error':relative}
    if relative>.001: raise RuntimeError(report)
    Path(a.output+'.parity.json').write_text(json.dumps(report,indent=2))
    print(report)
