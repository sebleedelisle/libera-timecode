# Timecode Jitter Report

Generated on 2026-05-01 with:

```sh
./build/libera_timecode_jitter \
  --duration 600 \
  --rates 0.95,1.0,1.05 \
  --cpu-workers 4 \
  --network-mbps 50 \
  --max-p99-jitter-ms 2 \
  --max-jitter-ms 10 \
  --max-mean-jitter-ms 1 \
  --outlier-ms 1 \
  --report reports/timecode-jitter/jitter-30min.csv \
  --outliers reports/timecode-jitter/jitter-30min-outliers.csv
```

The run passed with direct measurement enabled for:

- Art-Net UDP loopback frame cadence.
- SNTC UDP loopback frame cadence.
- MIDI MTC quarter-frame send cadence.
- SMPTE LTC decoded from generated audio samples.

All rows reported `duplicates=0`, `backwards=0`, `parse_errors=0`, and `outliers=0`.
The outlier CSV contains only the header because no interval exceeded the 1 ms outlier threshold.

Worst measured max absolute jitter in the run was 0.583958 ms on SNTC at 1.0x.
