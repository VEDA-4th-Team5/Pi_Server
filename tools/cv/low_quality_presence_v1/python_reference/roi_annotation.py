from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import html
import json
from collections import defaultdict, deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


ANNOTATION_FIELDS = [
    "annotation_id",
    "sample_category",
    "source_path",
    "source_uri",
    "sha256",
    "width",
    "height",
    "group_id",
    "ground_truth",
    "split",
    "plate_status",
    "plate_bbox",
    "plate_quad",
    "left_icon_bbox",
    "right_icon_bbox",
    "suggested_plate_bbox",
    "environment",
    "failure_type",
    "annotator",
    "review_status",
]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_box(value: str | list | None) -> list[int]:
    if isinstance(value, list):
        return [int(round(float(v))) for v in value]
    text = (value or "").strip()
    if not text:
        return []
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        return []
    return [int(round(float(v))) for v in parsed] if isinstance(parsed, list) else []


def xywh_to_xyxy(box: list[int], width: int, height: int) -> list[int]:
    if len(box) != 4:
        return []
    x, y, w, h = box
    return [max(0, x), max(0, y), min(width, x + w), min(height, y + h)]


def uniform_take(rows: list[dict[str, str]], count: int) -> list[dict[str, str]]:
    if len(rows) <= count:
        return list(rows)
    if count <= 1:
        return [rows[0]] if count else []
    indices = [round(i * (len(rows) - 1) / (count - 1)) for i in range(count)]
    return [rows[index] for index in indices]


def round_robin_by_reason(rows: list[dict[str, str]], count: int) -> list[dict[str, str]]:
    groups: dict[str, deque] = defaultdict(deque)
    for row in sorted(
        rows,
        key=lambda item: (-float(item.get("confidence") or 0.0), item.get("source_path") or ""),
    ):
        groups[(row.get("review_reason") or "unknown").strip()].append(row)
    selected: list[dict[str, str]] = []
    keys = sorted(groups)
    while len(selected) < count and keys:
        active = []
        for key in keys:
            if groups[key] and len(selected) < count:
                selected.append(groups[key].popleft())
            if groups[key]:
                active.append(key)
        keys = active
    return selected


def select_samples(
    detail_rows: list[dict[str, str]],
    fp_count: int = 400,
    review_count: int = 250,
    general_count: int = 200,
) -> list[tuple[str, dict[str, str]]]:
    ev = sorted(
        (row for row in detail_rows if row.get("ground_truth") == "EV"),
        key=lambda row: row.get("source_path") or "",
    )
    fp = sorted(
        (
            row
            for row in detail_rows
            if row.get("ground_truth") == "NON_EV" and row.get("predicted_class") == "EV"
        ),
        key=lambda row: (-float(row.get("confidence") or 0.0), row.get("source_path") or ""),
    )[:fp_count]
    review_pool = [
        row
        for row in detail_rows
        if row.get("ground_truth") == "NON_EV" and row.get("predicted_class") == "REVIEW"
    ]
    review = round_robin_by_reason(review_pool, review_count)
    used = {row.get("source_path") for row in ev + fp + review}
    general_pool = sorted(
        (
            row
            for row in detail_rows
            if row.get("ground_truth") == "NON_EV"
            and row.get("predicted_class") == "NON_EV"
            and row.get("source_path") not in used
        ),
        key=lambda row: row.get("source_path") or "",
    )
    general = uniform_take(general_pool, general_count)
    return (
        [("ev_all", row) for row in ev]
        + [("hard_negative_fp", row) for row in fp]
        + [("non_ev_review", row) for row in review]
        + [("general_non_ev", row) for row in general]
    )


def make_annotation_rows(selected: list[tuple[str, dict[str, str]]]) -> list[dict]:
    result = []
    for index, (category, source) in enumerate(selected, start=1):
        path = Path(source["source_path"]).resolve()
        width = int(source.get("width") or 0)
        height = int(source.get("height") or 0)
        suggested = xywh_to_xyxy(
            parse_box(source.get("color_candidate_bbox")), width, height
        )
        result.append(
            {
                "annotation_id": f"roi-{index:04d}",
                "sample_category": category,
                "source_path": str(path),
                "source_uri": path.as_uri(),
                "sha256": source.get("sha256") or (sha256_file(path) if path.exists() else ""),
                "width": width,
                "height": height,
                "group_id": f"plate:{path.stem}",
                "ground_truth": source.get("ground_truth") or "UNKNOWN",
                "split": "unassigned",
                "plate_status": "",
                "plate_bbox": [],
                "plate_quad": [],
                "left_icon_bbox": [],
                "right_icon_bbox": [],
                "suggested_plate_bbox": suggested,
                "environment": "unlabeled",
                "failure_type": source.get("review_reason") or "",
                "annotator": "",
                "review_status": "pending",
            }
        )
    return result


def box_valid(box: list, width: int, height: int) -> bool:
    if len(box) != 4:
        return False
    x1, y1, x2, y2 = [float(value) for value in box]
    return 0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height


def quad_valid(quad: list, width: int, height: int) -> bool:
    if len(quad) != 8:
        return False
    points = list(zip(quad[0::2], quad[1::2]))
    return all(0 <= float(x) <= width and 0 <= float(y) <= height for x, y in points)


def validate_annotations(rows: list[dict], require_complete: bool = False) -> dict:
    errors: list[dict] = []
    warnings: list[dict] = []
    seen = set()
    groups: dict[str, set[str]] = defaultdict(set)
    allowed_status = {"", "full", "partial", "unusable", "not_visible"}
    allowed_review = {"pending", "approved", "rejected", "skipped"}
    for row in rows:
        item_id = row.get("annotation_id", "")
        source = row.get("source_path", "")
        width, height = int(row.get("width") or 0), int(row.get("height") or 0)
        if source in seen:
            errors.append({"annotation_id": item_id, "reason": "duplicate_source"})
        seen.add(source)
        if not Path(source).exists():
            errors.append({"annotation_id": item_id, "reason": "missing_source"})
        if width <= 0 or height <= 0:
            errors.append({"annotation_id": item_id, "reason": "invalid_dimensions"})
        review_status = row.get("review_status") or "pending"
        plate_status = row.get("plate_status") or ""
        if review_status not in allowed_review:
            errors.append({"annotation_id": item_id, "reason": "invalid_review_status"})
        if plate_status not in allowed_status:
            errors.append({"annotation_id": item_id, "reason": "invalid_plate_status"})
        if require_complete and review_status == "pending":
            errors.append({"annotation_id": item_id, "reason": "review_pending"})
        for key in ("plate_bbox", "left_icon_bbox", "right_icon_bbox"):
            box = parse_box(row.get(key))
            if box and not box_valid(box, width, height):
                errors.append({"annotation_id": item_id, "reason": f"invalid_{key}"})
        quad = parse_box(row.get("plate_quad"))
        if quad and not quad_valid(quad, width, height):
            errors.append({"annotation_id": item_id, "reason": "invalid_plate_quad"})
        if review_status == "approved" and plate_status in {"full", "partial"}:
            if not parse_box(row.get("plate_bbox")) and not quad:
                errors.append({"annotation_id": item_id, "reason": "approved_plate_roi_missing"})
            if row.get("ground_truth") == "EV" and (
                not parse_box(row.get("left_icon_bbox"))
                or not parse_box(row.get("right_icon_bbox"))
            ):
                warnings.append({"annotation_id": item_id, "reason": "ev_icon_roi_incomplete"})
        group_id = row.get("group_id") or ""
        split = row.get("split") or "unassigned"
        if group_id and split != "unassigned":
            groups[group_id].add(split)
    leak_groups = sorted(group for group, splits in groups.items() if len(splits) > 1)
    for group in leak_groups:
        errors.append({"annotation_id": "", "reason": f"group_split_leak:{group}"})
    counts = defaultdict(int)
    for row in rows:
        counts[row.get("review_status") or "pending"] += 1
    return {
        "row_count": len(rows),
        "error_count": len(errors),
        "warning_count": len(warnings),
        "pending_count": counts["pending"],
        "approved_count": counts["approved"],
        "rejected_count": counts["rejected"],
        "skipped_count": counts["skipped"],
        "group_split_leak_count": len(leak_groups),
        "complete": require_complete and not errors,
        "errors": errors,
        "warnings": warnings,
    }


def serializable_row(row: dict) -> dict:
    result = dict(row)
    for key in (
        "plate_bbox",
        "plate_quad",
        "left_icon_bbox",
        "right_icon_bbox",
        "suggested_plate_bbox",
    ):
        result[key] = parse_box(result.get(key))
    result["width"] = int(result.get("width") or 0)
    result["height"] = int(result.get("height") or 0)
    return result


def csv_row(row: dict) -> dict:
    result = serializable_row(row)
    for key in (
        "plate_bbox",
        "plate_quad",
        "left_icon_bbox",
        "right_icon_bbox",
        "suggested_plate_bbox",
    ):
        result[key] = json.dumps(result[key], ensure_ascii=False)
    return result


def write_annotation_csv(path: Path, rows: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=ANNOTATION_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow(csv_row(row))


def build_embedded_image_map(rows: list[dict]) -> dict[str, str]:
    image_data: dict[str, str] = {}
    for row in rows:
        path = Path(row["source_path"])
        suffix = path.suffix.lower()
        mime = "image/jpeg" if suffix in {".jpg", ".jpeg"} else "image/png"
        encoded = base64.b64encode(path.read_bytes()).decode("ascii")
        image_data[row["annotation_id"]] = f"data:{mime};base64,{encoded}"
    return image_data


def build_html(rows: list[dict]) -> str:
    data = json.dumps([serializable_row(row) for row in rows], ensure_ascii=False).replace("</", "<\\/")
    image_data = json.dumps(build_embedded_image_map(rows), ensure_ascii=False).replace("</", "<\\/")
    return f"""<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><title>EV ROI/Icon Annotation</title>
<style>
body{{margin:0;font-family:Arial,sans-serif;background:#111827;color:#e5e7eb}}header{{position:sticky;top:0;background:#0f172a;padding:10px;z-index:2}}button,select,input{{margin:3px;padding:7px}}.grid{{display:grid;grid-template-columns:330px 1fr;gap:12px;padding:12px}}.panel{{background:#1f2937;padding:12px;border-radius:8px}}canvas{{max-width:100%;background:#000;cursor:crosshair}}.row{{margin:6px 0}}.active{{outline:3px solid #f59e0b}}.ok{{color:#34d399}}.warn{{color:#fbbf24}}code{{font-size:12px;word-break:break-all}}textarea{{width:100%;height:70px}}</style></head>
<body><header><b>EV ROI/Icon Annotation</b> <span id="progress"></span>
<select id="category"><option value="all">all</option><option>ev_all</option><option>hard_negative_fp</option><option>non_ev_review</option><option>general_non_ev</option></select>
<select id="reviewFilter"><option value="all">all status</option><option>pending</option><option>approved</option><option>rejected</option><option>skipped</option></select>
<button onclick="move(-1)">← 이전</button><button onclick="move(1)">다음 →</button>
<button onclick="exportJson()">JSON 내보내기</button><button onclick="exportCsv()">CSV 내보내기</button></header>
<div class="grid"><section class="panel">
<div><b id="itemTitle"></b></div><div><code id="source"></code></div>
<div class="row">GT: <b id="gt"></b> / category: <span id="cat"></span></div>
<div class="row">그리기 모드:</div>
<button id="plateBtn" onclick="setMode('plate_bbox')">1 Plate bbox</button>
<button id="leftBtn" onclick="setMode('left_icon_bbox')">2 Left icon</button>
<button id="rightBtn" onclick="setMode('right_icon_bbox')">3 Right icon</button>
<button id="quadBtn" onclick="setMode('plate_quad')">4 Plate quad</button>
<button onclick="useSuggestion()">추천 bbox 사용</button><button onclick="clearMode()">현재 ROI 지우기</button>
<div class="row">번호판 상태:</div>
<button onclick="setPlateStatus('full')">F full</button><button onclick="setPlateStatus('partial')">P partial</button>
<button onclick="setPlateStatus('unusable')">U unusable</button><button onclick="setPlateStatus('not_visible')">N not visible</button>
<div class="row"><label>환경 <input id="environment" onchange="saveFields()"></label></div>
<div class="row"><label>실패 유형 <input id="failure" onchange="saveFields()"></label></div>
<div class="row"><label>검수자 <input id="annotator" onchange="saveFields()"></label></div>
<div class="row"><button onclick="setReview('approved')">A 승인</button><button onclick="setReview('rejected')">R 거절</button><button onclick="setReview('skipped')">S 보류</button></div>
<pre id="state"></pre><div id="message" class="warn"></div>
<p>단축키: 1/2/3/4 모드, F/P/U/N 상태, A 승인, R 거절, S 보류, ←/→ 이동.</p>
</section><section class="panel"><canvas id="canvas"></canvas></section></div>
<script>
const seed={data}; const imageData={image_data}; const key='ev-roi-icon-002-step01';
let rows=JSON.parse(localStorage.getItem(key)||'null')||seed; let filtered=[]; let pos=0; let mode='plate_bbox'; let start=null; let quad=[];
const canvas=document.getElementById('canvas'),ctx=canvas.getContext('2d'),img=new Image();
function refreshFilter(){{const c=category.value,r=reviewFilter.value;filtered=rows.map((x,i)=>[x,i]).filter(([x])=>(c==='all'||x.sample_category===c)&&(r==='all'||x.review_status===r)).map(x=>x[1]);if(pos>=filtered.length)pos=Math.max(0,filtered.length-1);render();}}
function current(){{return rows[filtered[pos]??0]}}
function store(){{localStorage.setItem(key,JSON.stringify(rows));updateState();}}
function setMode(x){{mode=x;quad=[];document.querySelectorAll('button').forEach(b=>b.classList.remove('active'));const id={{plate_bbox:'plateBtn',left_icon_bbox:'leftBtn',right_icon_bbox:'rightBtn',plate_quad:'quadBtn'}}[x];document.getElementById(id).classList.add('active');}}
function render(){{if(!filtered.length){{filtered=rows.map((_,i)=>i)}}const x=current();img.onload=()=>{{canvas.width=img.naturalWidth;canvas.height=img.naturalHeight;draw();message.textContent=''}};img.onerror=()=>{{canvas.width=Math.max(640,x.width||640);canvas.height=Math.max(360,x.height||360);ctx.clearRect(0,0,canvas.width,canvas.height);message.textContent='이미지를 불러오지 못했습니다: '+x.source_path}};img.src=imageData[x.annotation_id]||x.source_uri;itemTitle.textContent=`${{x.annotation_id}} (${{pos+1}}/${{filtered.length}})`;source.textContent=x.source_path;gt.textContent=x.ground_truth;cat.textContent=x.sample_category;environment.value=x.environment||'';failure.value=x.failure_type||'';annotator.value=x.annotator||'';progress.textContent=`승인 ${{rows.filter(r=>r.review_status==='approved').length}} / 전체 ${{rows.length}}`;updateState();}}
function box(key,color,dash=false){{const b=current()[key]||[];if(b.length!==4)return;ctx.save();ctx.strokeStyle=color;ctx.lineWidth=Math.max(2,canvas.width/300);if(dash)ctx.setLineDash([10,8]);ctx.strokeRect(b[0],b[1],b[2]-b[0],b[3]-b[1]);ctx.restore();}}
function draw(){{ctx.drawImage(img,0,0);box('suggested_plate_bbox','#f59e0b',true);box('plate_bbox','#22c55e');box('left_icon_bbox','#38bdf8');box('right_icon_bbox','#f472b6');const q=current().plate_quad||[];if(q.length===8){{ctx.strokeStyle='#fde047';ctx.beginPath();ctx.moveTo(q[0],q[1]);for(let i=2;i<8;i+=2)ctx.lineTo(q[i],q[i+1]);ctx.closePath();ctx.stroke()}}}}
function point(ev){{const r=canvas.getBoundingClientRect();return [Math.round((ev.clientX-r.left)*canvas.width/r.width),Math.round((ev.clientY-r.top)*canvas.height/r.height)]}}
canvas.onmousedown=e=>{{if(mode==='plate_quad')return;start=point(e)}};canvas.onmouseup=e=>{{if(!start||mode==='plate_quad')return;const p=point(e);current()[mode]=[Math.min(start[0],p[0]),Math.min(start[1],p[1]),Math.max(start[0],p[0]),Math.max(start[1],p[1])];start=null;store();draw()}};
canvas.onclick=e=>{{if(mode!=='plate_quad')return;const p=point(e);quad.push(...p);if(quad.length===8){{current().plate_quad=quad.slice();quad=[];store();draw()}}}};
function useSuggestion(){{current().plate_bbox=[...(current().suggested_plate_bbox||[])];store();draw()}}
function clearMode(){{current()[mode]=[];quad=[];store();draw()}}
function setPlateStatus(x){{current().plate_status=x;store()}}
function setReview(x){{current().review_status=x;store();refreshFilter()}}
function saveFields(){{Object.assign(current(),{{environment:environment.value,failure_type:failure.value,annotator:annotator.value}});store()}}
function move(d){{saveFields();pos=Math.max(0,Math.min(filtered.length-1,pos+d));render()}}
function updateState(){{const x=current();if(!x)return;state.textContent=JSON.stringify({{plate_status:x.plate_status,review_status:x.review_status,plate_bbox:x.plate_bbox,plate_quad:x.plate_quad,left_icon_bbox:x.left_icon_bbox,right_icon_bbox:x.right_icon_bbox}},null,2);message.textContent=(x.review_status==='approved'&&!x.plate_status)?'승인 전 번호판 상태를 선택하세요':''}}
function download(name,text,type){{const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([text],{{type}}));a.download=name;a.click();URL.revokeObjectURL(a.href)}}
function exportJson(){{saveFields();download('human_annotations.json',JSON.stringify(rows,null,2),'application/json')}}
function csvCell(v){{const s=Array.isArray(v)?JSON.stringify(v):String(v??'');return '"'+s.replaceAll('"','""')+'"'}}
function exportCsv(){{saveFields();const fields={json.dumps(ANNOTATION_FIELDS)};const lines=[fields.join(',')].concat(rows.map(r=>fields.map(f=>csvCell(r[f])).join(',')));download('human_annotations.csv',lines.join('\\n'),'text/csv')}}
category.onchange=refreshFilter;reviewFilter.onchange=refreshFilter;document.onkeydown=e=>{{if(['INPUT','TEXTAREA'].includes(document.activeElement.tagName))return;const k=e.key.toLowerCase();if(k==='1')setMode('plate_bbox');else if(k==='2')setMode('left_icon_bbox');else if(k==='3')setMode('right_icon_bbox');else if(k==='4')setMode('plate_quad');else if(k==='f')setPlateStatus('full');else if(k==='p')setPlateStatus('partial');else if(k==='u')setPlateStatus('unusable');else if(k==='n')setPlateStatus('not_visible');else if(k==='a')setReview('approved');else if(k==='r')setReview('rejected');else if(k==='s')setReview('skipped');else if(e.key==='ArrowLeft')move(-1);else if(e.key==='ArrowRight')move(1)}};setMode('plate_bbox');refreshFilter();
</script></body></html>"""


def prepare(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    details = read_csv(Path(args.details).resolve())
    selected = select_samples(details, args.fp_count, args.review_count, args.general_count)
    rows = make_annotation_rows(selected)
    write_annotation_csv(output_dir / "manifest.csv", rows)
    write_annotation_csv(output_dir / "details.csv", rows)
    (output_dir / "annotation_seed.json").write_text(
        json.dumps([serializable_row(row) for row in rows], ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    (output_dir / "roi_annotation.html").write_text(build_html(rows), encoding="utf-8")
    validation = validate_annotations(rows, require_complete=False)
    counts = defaultdict(int)
    for row in rows:
        counts[row["sample_category"]] += 1
    environment_rows = [
        {"environment": "unlabeled", "sample_category": key, "count": value}
        for key, value in sorted(counts.items())
    ]
    with (output_dir / "environment_summary.csv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=["environment", "sample_category", "count"])
        writer.writeheader()
        writer.writerows(environment_rows)
    summary = {
        "experiment_id": "ev-roi-icon-002-step01-roi-annotation",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": "review_pending",
        "selected_total": len(rows),
        "counts": dict(counts),
        "validation": validation,
        "source_details": str(Path(args.details).resolve()),
        "source_data_modified": False,
        "performance_claim": False,
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if validation["error_count"] == 0 else 1


def merge(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir).resolve()
    seed = read_csv(Path(args.seed_manifest).resolve())
    updates = json.loads(Path(args.annotations_json).read_text(encoding="utf-8-sig"))
    update_map = {row["annotation_id"]: row for row in updates}
    merged = []
    for seed_row in seed:
        update = update_map.get(seed_row["annotation_id"], {})
        row = {**seed_row, **{key: update[key] for key in ANNOTATION_FIELDS if key in update}}
        merged.append(serializable_row(row))
    output_dir.mkdir(parents=True, exist_ok=True)
    write_annotation_csv(output_dir / "approved_manifest.csv", merged)
    validation = validate_annotations(merged, require_complete=True)
    (output_dir / "validation.json").write_text(
        json.dumps(validation, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(validation, ensure_ascii=False, indent=2))
    return 0 if validation["error_count"] == 0 else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser("EV plate ROI/icon annotation workflow")
    sub = parser.add_subparsers(dest="command", required=True)
    prepare_parser = sub.add_parser("prepare")
    prepare_parser.add_argument("--details", required=True)
    prepare_parser.add_argument("--output-dir", required=True)
    prepare_parser.add_argument("--fp-count", type=int, default=400)
    prepare_parser.add_argument("--review-count", type=int, default=250)
    prepare_parser.add_argument("--general-count", type=int, default=200)
    prepare_parser.set_defaults(func=prepare)
    merge_parser = sub.add_parser("merge")
    merge_parser.add_argument("--seed-manifest", required=True)
    merge_parser.add_argument("--annotations-json", required=True)
    merge_parser.add_argument("--output-dir", required=True)
    merge_parser.set_defaults(func=merge)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())

