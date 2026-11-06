# Diagnostic views (dossier Sec. 26)

"Publish spectra, harmonic levels versus drive, intermodulation spectra, time-domain residuals, burst envelopes, rail/cathode trajectories where available, impedance magnitude/phase and acoustic position
comparisons. One aggregate error score can hide a narrow frequency defect or a serious recovery mismatch."

These are views of the **model** (a documented approximation of the modern '57 Custom Deluxe), not of an amplifier. They exist so that the same views can be drawn from hardware captures and compared by eye
before any single number is trusted. Regenerate them with

```bash
python3 scripts/plot_diagnostics.py --build build        # runs cd57_diagnostics from the build directory, then plots (needs numpy + matplotlib)
```

| Figure | Sec. 26 view | What it shows |
|---|---|---|
| `figures/fig01_response_controls.png` | clean response, control interaction | jack EMF -> speaker terminals, magnitude and phase, for the tone control (0-12), the Instrument volume, the four jack states, and the Mic volume with the guitar in Instrument 1 (the silent channel loads the driven one by ~17 dB at full) |
| `figures/fig02_impedance.png` | impedance magnitude / phase | input impedance at each jack (1 MOhm / 136 kOhm of Sec. 02, reproduced by the circuit); the amplifier's output impedance and the ASSUMED speaker model's impedance (Sec. 17: the load is complex) |
| `figures/fig03_harmonics_vs_drive.png` | harmonic levels versus drive | H2-H5 (dBc) and THD against output power at 1 kHz; the documented 12.5 W / 5 % marked as a calibration anchor |
| `figures/fig04_spectra_imd.png` | spectra, intermodulation spectra | a 1 kHz tone at three levels and a 700 Hz + 1 kHz two-tone with the product lines labelled |
| `figures/fig05_waveforms.png` | time-domain | the terminal-voltage cycle at three levels with the input |
| `figures/fig06_oversampling_residual.png` | time-domain residuals | residual of 1x / 2x / 4x oversampling against an 8x render of the same model (3.1 kHz, 60 mV: far above the rated drive, so these are worst-case residuals; `docs/NUMERICS_REPORT.md` has the systematic study) |
| `figures/fig07_burst_trajectories.png` | burst envelopes, rail / cathode trajectories | around a 150 ms, 40 mV burst: B+1/2/3, the shared 6V6 cathode, both power-valve grids (coupling-capacitor charge), the cathodyne nodes, the first-stage plate, and the quiet 1 kHz probe's gain dip and recovery |
| `figures/fig08_valve_conduction.png` | per-valve current (Sec. 10) | plate currents over one cycle at three levels, conduction angle and overlap versus drive, bias shift and rail sag; `CONDUCTION_REPORT.md` has the table |
| `figures/fig09_acoustic_positions.png` | acoustic position comparison | the PROVISIONAL acoustic operator at three microphone positions against the terminal response |
| `figures/fig10_sensitivity.png` | which unknowns matter | one-parameter-at-a-time sensitivity (Sec. 13), top 26 of 41 parameters; `SENSITIVITY_REPORT.md` has the full table |
| `figures/fig11_capture_matrix_overview.png` | the capture programme | small-signal gain and clipping onset over the 4 x 27 conditions of the Sec. 25 matrix |

Data behind the figures: `cd57_diagnostics --out DIR` writes `response.csv`, `impedance.csv`, `harmonics_vs_drive.csv`, `spectra.csv`, `waveforms.csv`, `residual.csv`, `burst.csv`, `acoustic.csv`; the reference
CSVs of the regression assets are in `data/reference/`.
