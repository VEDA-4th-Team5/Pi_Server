# Python Reference

이 폴더는 C++ 포트의 동작 비교용 reference source다.

`low_quality_independent_holdout_predict.py`는 독립 holdout prediction generator이며, 운영 daemon이 아니다. audit manifest, icon template manifest, OCR runtime이 필요하고, human GT를 runtime 입력으로 사용하지 않도록 설계되어 있다.

따라서 다음 용도로만 사용한다.

- C++ port와 candidate/rectification/decision 비교
- model feature 순서 확인
- golden image 결과 생성
- 독립 holdout 평가 재현

서버 runtime에서는 manifest와 GT를 제거하고, template/feature를 model bundle에 명시적으로 export해야 한다.

