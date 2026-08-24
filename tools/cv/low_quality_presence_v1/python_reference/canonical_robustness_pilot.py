from __future__ import annotations

import argparse
import csv
import json
import statistics
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np

from .background_color_pilot import central_background_features
from .icon_template_bank import build_templates
from .left_icon_detector import detect as detect_left
from .right_icon_detector import detect as detect_right
from .roi_annotation import read_csv
from .top3_localization import read_image


def degrade(image: np.ndarray, environment: str) -> np.ndarray:
    h, w = image.shape[:2]
    if environment == "normal": return image.copy()
    if environment == "downscale_050": return cv2.resize(cv2.resize(image,(w//2,h//2),interpolation=cv2.INTER_AREA),(w,h),interpolation=cv2.INTER_CUBIC)
    if environment == "gaussian_blur_07": return cv2.GaussianBlur(image,(7,7),0)
    if environment == "motion_blur_09":
        kernel=np.zeros((9,9),np.float32);kernel[4,:]=1/9;return cv2.filter2D(image,-1,kernel)
    if environment == "defocus_blur_07": return cv2.GaussianBlur(image,(7,7),2.5)
    if environment == "low_light":
        normalized=image.astype(np.float32)/255;return np.clip((normalized**1.9)*255,0,255).astype(np.uint8)
    if environment == "backlight":
        gradient=np.linspace(.75,1.7,w,dtype=np.float32)[None,:,None];return np.clip(image.astype(np.float32)*gradient,0,255).astype(np.uint8)
    if environment == "white_balance":
        gains=np.array([1.25,1.0,.78],np.float32);return np.clip(image.astype(np.float32)*gains,0,255).astype(np.uint8)
    if environment == "desaturation":
        hsv=cv2.cvtColor(image,cv2.COLOR_BGR2HSV);hsv[:,:,1]=(hsv[:,:,1].astype(np.float32)*.18).astype(np.uint8);return cv2.cvtColor(hsv,cv2.COLOR_HSV2BGR)
    if environment == "jpeg_q40":
        return cv2.imdecode(cv2.imencode('.jpg',image,[cv2.IMWRITE_JPEG_QUALITY,40])[1],cv2.IMREAD_COLOR)
    if environment == "perspective":
        src=np.float32([[0,0],[w-1,0],[w-1,h-1],[0,h-1]]);dst=np.float32([[7,3],[w-8,0],[w-2,h-4],[2,h-1]]);return cv2.warpPerspective(image,cv2.getPerspectiveTransform(src,dst),(w,h),borderMode=cv2.BORDER_REPLICATE)
    if environment == "partial_crop":
        crop=image[:,int(w*.08):int(w*.94)];return cv2.resize(crop,(w,h),interpolation=cv2.INTER_CUBIC)
    if environment == "glare":
        out=image.copy();overlay=out.copy();cv2.ellipse(overlay,(w//2,h//3),(w//5,h//4),0,0,360,(255,255,255),-1);return cv2.addWeighted(overlay,.45,out,.55,0)
    if environment == "ir_simulated_monochrome":
        gray=cv2.cvtColor(image,cv2.COLOR_BGR2GRAY);return cv2.cvtColor(gray,cv2.COLOR_GRAY2BGR)
    raise ValueError(f"unknown environment:{environment}")


def run(args: argparse.Namespace) -> int:
    records=read_csv(Path(args.records).resolve());summary=json.loads(Path(args.icon_summary).read_text(encoding="utf-8-sig"));cuts={"left":float(summary["metrics"]["left"]["threshold_diagnostic_only"]),"right":float(summary["metrics"]["right"]["threshold_diagnostic_only"])}
    eligible=[r for r in records if (r["icon_label"]=="present" and not r["exclusion_reason"]) or r["label_source"]=="non_ev_class_inferred_negative"]
    envs=["normal","downscale_050","gaussian_blur_07","motion_blur_09","defocus_blur_07","low_light","backlight","white_balance","desaturation","jpeg_q40","perspective","partial_crop","glare","ir_simulated_monochrome"]
    details=[];errors=[]
    template_cache = {}
    for source in eligible:
        cache_key = (source["side"], source["annotation_id"] if source["icon_label"] == "present" else "")
        if cache_key not in template_cache:
            template_cache[cache_key] = build_templates(eligible, source["side"], source["annotation_id"] if source["icon_label"] == "present" else None)
    for row in eligible:
        image=read_image(Path(row["canonical_path"]));
        if image is None: errors.append({"annotation_id":row["annotation_id"],"reason":"canonical_read_failed"});continue
        for environment in envs:
            started=time.perf_counter()
            try:
                degraded=degrade(image,environment);features=central_background_features(degraded);cache_key=(row["side"],row["annotation_id"] if row["icon_label"]=="present" else "");templates=template_cache[cache_key];result=detect_left(degraded,templates) if row["side"]=="left" else detect_right(degraded,templates);elapsed=(time.perf_counter()-started)*1000
                details.append({"annotation_id":row["annotation_id"],"ground_truth":row["ground_truth"],"side":row["side"],"label":"present" if row["icon_label"]=="present" else "absent","label_source":row["label_source"],"environment":environment,"background_color_score":f"{features['central_color_score']:.6f}","color_unavailable":features["color_unavailable"],"icon_score":f"{result['score']:.6f}","icon_detected":int(result["score"]>=cuts[row["side"]]),"processing_ms":f"{elapsed:.3f}","error_reason":""})
            except Exception as exc: errors.append({"annotation_id":row["annotation_id"],"side":row["side"],"environment":environment,"reason":f"{type(exc).__name__}:{exc}"})
    output=Path(args.output_dir).resolve();output.mkdir(parents=True,exist_ok=True);fields=list(details[0]) if details else ["annotation_id"]
    for name in ("manifest.csv","details.csv"):
        with (output/name).open("w",encoding="utf-8",newline="") as h:w=csv.DictWriter(h,fieldnames=fields);w.writeheader();w.writerows(details)
    env_summary=[]
    for environment in envs:
        for side in ("left","right"):
            group=[r for r in details if r["environment"]==environment and r["side"]==side];pos=[r for r in group if r["label"]=="present"];neg=[r for r in group if r["label"]=="absent"]
            recall=sum(int(r["icon_detected"]) for r in pos)/max(1,len(pos));fpr=sum(int(r["icon_detected"]) for r in neg)/max(1,len(neg));unavailable=sum(int(r["color_unavailable"]) for r in group)/max(1,len(group));status="data_insufficient" if environment=="ir_simulated_monochrome" else "conditional" if recall>=.5 else "failed"
            env_summary.append({"environment":environment,"side":side,"status":status,"roi_top3_recall":"N/A_canonical_input","positive_count":len(pos),"negative_count":len(neg),"icon_recall":recall,"icon_fpr":fpr,"color_unavailable_ratio":unavailable,"mean_ms":statistics.fmean(float(r["processing_ms"]) for r in group) if group else 0.0})
    with (output/"environment_summary.csv").open("w",encoding="utf-8",newline="") as h:w=csv.DictWriter(h,fieldnames=list(env_summary[0]));w.writeheader();w.writerows(env_summary)
    result={"experiment_id":args.experiment_id,"generated_at_utc":datetime.now(timezone.utc).isoformat(),"status":"pilot_complete" if not errors else "pilot_failed","detail_rows":len(details),"error_count":len(errors),"errors":errors,"environment_status_counts":dict((x,sum(r["status"]==x for r in env_summary)) for x in {r["status"] for r in env_summary}),"real_ir_evaluated":False,"formal_step7_gate_passed":False,"performance_claim":False}
    (output/"summary.json").write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding="utf-8");print(json.dumps(result,ensure_ascii=False,indent=2));return 0 if not errors else 1


def parser():
    p=argparse.ArgumentParser("Canonical plate robustness Pilot");p.add_argument("--records",required=True);p.add_argument("--icon-summary",required=True);p.add_argument("--output-dir",required=True);p.add_argument("--experiment-id",default="ev-roi-icon-002-step07-canonical-robustness-pilot107");return p
if __name__=="__main__":raise SystemExit(run(parser().parse_args()))

