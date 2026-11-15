# Project 3D models

Two footprints look here for manufacturer STEP files that are not in the stock KiCad library:

| Ref | Part | Expected file | Source |
|---|---|---|---|
| J1 | Molex Pico-SPOX 87437-0473 | `874370473.stp` (included) | Molex model via TraceParts; https://www.molex.com/en-us/products/part-detail/874370473 |
| S3 | C&K JS102011SAQN | `JS102011SAQN.step` | https://www.ckswitches.com/products/switches/product-details/Slide/JS/ → 3D model |

J1 is included and lines up with the footprint outline. S3 is still missing: put its file in this folder under the
expected name. S3's offset has not been checked against the C&K file and may need adjusting in the footprint's
3D Models tab.
