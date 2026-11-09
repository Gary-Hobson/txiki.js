# Opcode diagnostic profile

Dynamic dispatch counts across the runtime through completion of the Web Tooling Benchmark. Consecutive pairs can cross calls. Counts are not time or hardware cycles; instrumented throughput is not scored.
Total dispatches: 4,705,698,147; pairs: 4,705,698,146.

## Top 20 opcodes

| Opcode | Count | Share |
|---|---:|---:|
| if_false8 | 381,809,496 | 8.11% |
| get_field | 349,237,622 | 7.42% |
| get_loc8 | 318,755,881 | 6.77% |
| get_arg0 | 247,498,726 | 5.26% |
| get_loc0 | 207,270,297 | 4.40% |
| strict_eq | 182,815,712 | 3.88% |
| get_field2 | 153,928,305 | 3.27% |
| dup | 146,776,087 | 3.12% |
| get_loc1 | 127,599,079 | 2.71% |
| call_method | 126,281,058 | 2.68% |
| if_true8 | 119,836,725 | 2.55% |
| drop | 108,168,663 | 2.30% |
| lt | 106,264,735 | 2.26% |
| get_loc2 | 102,836,788 | 2.19% |
| get_array_el | 102,287,066 | 2.17% |
| get_length | 92,437,050 | 1.96% |
| return | 78,444,412 | 1.67% |
| get_var_ref0 | 77,517,631 | 1.65% |
| get_arg1 | 75,457,984 | 1.60% |
| push_i8 | 74,686,523 | 1.59% |

## Top 20 consecutive dispatch pairs

| Pair | Count | Share |
|---|---:|---:|
| strict_eq → if_false8 | 113,408,501 | 2.41% |
| get_loc8 → get_loc8 | 77,354,708 | 1.64% |
| lt → if_false8 | 76,705,000 | 1.63% |
| get_loc0 → get_field | 61,629,958 | 1.31% |
| dup → if_true8 | 56,756,764 | 1.21% |
| call_method → push_this | 53,818,366 | 1.14% |
| if_false8 → get_loc8 | 52,247,035 | 1.11% |
| goto8 → get_loc8 | 48,321,897 | 1.03% |
| get_length → lt | 47,982,294 | 1.02% |
| get_arg0 → get_field | 45,305,116 | 0.96% |
| get_loc8 → strict_eq | 44,329,372 | 0.94% |
| if_false8 → get_loc0 | 43,391,511 | 0.92% |
| if_true8 → drop | 43,141,875 | 0.92% |
| get_field → get_field | 42,664,313 | 0.91% |
| get_field → get_loc8 | 42,075,580 | 0.89% |
| get_loc8 → get_field | 42,002,762 | 0.89% |
| inc_loc → goto8 | 40,897,355 | 0.87% |
| get_arg0 → push_i8 | 40,105,887 | 0.85% |
| if_false8 → get_arg0 | 39,306,284 | 0.84% |
| push_i8 → strict_eq | 39,304,252 | 0.84% |
