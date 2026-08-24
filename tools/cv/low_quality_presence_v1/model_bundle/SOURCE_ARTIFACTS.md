# Model Bundle Source Artifacts

이 폴더의 JSON은 현재 개발 후보에서 복사한 원본 summary/artifact다. 보고서의 학습 결과를 재튜닝하거나 운영 승인으로 해석하지 않는다.

| 전달 파일 | 원본 |
|---|---|
| `selector_summary.json` | `cv_logic_python_v2/output/reports/low-quality-candidate-selector-007/summary.json` |
| `side_model_summary.json` | `cv_logic_python_v2/output/reports/low-quality-presence-rectification-003/summary.json` |
| `spatial_summary.json` | `cv_logic_python_v2/output/reports/low-quality-spatial-icon-descriptor-013/summary.json` |
| `guard_summary.json` | `cv_logic_python_v2/output/reports/low-quality-runtime-icon-observability-guard-014-rerun-001/summary.json` |
| `default_thresholds.json` | `vehicle_color_prescreen_v1/config/default_thresholds.json` |
| `runtime_template_cache.json` | `runtime/build_runtime_template_cache.py`로 생성한 개발용 descriptor cache |

## 상태 제한

- `operational_candidate`: false
- `real_ir_evaluated`: false
- `independent_holdout_evaluated`: false
- `performance_claim`: false
- `runtime_template_cache_bootstrap`: true
- `runtime_template_cache_negative_source`: inferred-negative, development-only

서버에 배포할 때는 `shadow-only` configuration으로만 활성화한다.
