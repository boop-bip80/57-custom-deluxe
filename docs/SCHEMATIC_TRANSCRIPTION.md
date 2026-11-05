# Independent transcription of the reference schematic (dossier Fig. 7)

Purpose: dossier milestone B (Sec. 29) asks for the connection graph and component register to be independently
reviewed against the reference. This file and `data/netlist/schematic_register.json` are a transcription of the
drawing made **before** the repository netlist (`data/netlist/cd57_nominal.net`), `docs/NETLIST.md` or `src/dsp/`
were opened. The machine-readable register is the authority; this document explains how it was read. The graph
comparison against the netlist is in `scripts/check_netlist_against_register.py` and the section "Comparison with
the repository netlist" at the end of this file.

Evidence label for everything transcribed from the drawing: **HISTORICAL** (original 1957 5E3, annotated by Rob
Robinette; dossier Sec. 05 and 14 say it is a topology reference, not an authenticated modern PR 728 schematic).
Items marked DERIVED follow from the drawing plus stated reasoning.

Method: Fig. 7 (1464 x 899 px) was cropped and upscaled region by region (input jacks, V1, volume/tone network,
V2A/V2B, 6V6 pair, output transformer and speaker jacks, supply, mains primary) and every wire was traced.
The grey thick lines are the "signal path" highlight and are ordinary wires. Dossier Fig. 8 (wiring layout) was
used only to resolve jack-lug wiring and to cross-check printed ratings.

## 1. Net dictionary

| Net | Meaning |
|---|---|
| GND | chassis ground; every ground symbol, OT secondary cold end, HV centre tap, safety ground |
| BH_A, NH_A | Bright / Normal "Hi" node: Hi jack tip lug, 1 MEG top, upper 68k top, wire to the Lo jack's switch lug |
| BL_L, NL_L | Bright / Normal "Lo" node: Lo jack tip lug, lower 68k bottom |
| V1B_G, V1A_G | V1 grid nodes between the two 68k stoppers of each channel |
| V1B_P, V1A_P | V1 plates |
| V1B_C, V1A_C | coupling-cap output = volume-pot wiper (V1B_C also carries the 500 pF Bright cap) |
| K1 | V1 shared cathode |
| TONE_T / TONE_B | tone pot end toward the Bright cap / toward the tone cap |
| MIX | common mix node, V2A grid |
| V2A_P, V2A_K | V2A plate, cathode |
| V2B_G, V2B_P, V2B_K, V2B_T | cathodyne grid, plate ("Inverted Output"), cathode ("Non-Inverted Output"), tail node (1500/56K/1 MEG junction) |
| INV_G3, V3_G / NON_G4, V4_G | 6V6 grid-leak/stopper input node and grid, per tube |
| K3 | "Bias Voltage" node, both 6V6 cathodes |
| OT_PA, OT_PB | 6V6 plates = OT primary ends |
| B+1, B+2, B+3 | filter nodes as labelled on the drawing; B+1 is also "HT or B+ High Voltage DC" |
| SPK_HOT | OT secondary hot end, speaker and extra jack tips |
| RECT_PA, RECT_PB, RECT_F | 5Y3 plates and the second filament lead |
| MAINS_HOT, MAINS_NEUT, PRI_A, PRI_B | mains prongs and transformer primary ends |

## 2. Component register (every printed part)

Values exactly as printed in the drawing; capacitor labels are "uF-working volts" without units.

### Input jacks and grid networks (identical for Bright = V1B and Normal = V1A)

| Part | Printed | Between | Function |
|---|---|---|---|
| 1 MEG "Input Resistor" (x2) | 1 MEG | BH_A/NH_A to GND | Hi input / grid-leak path |
| 68k "Grid Stopper" upper (x2) | 68k | BH_A/NH_A to V1B_G/V1A_G | Hi-side stopper |
| 68k "Grid Stopper" lower (x2) | 68k | BL_L/NL_L to V1B_G/V1A_G | Lo-side stopper |

Jacks (see the plug-state table below): Hi jack tip lug = BH_A, switch lug = GND; Lo jack tip lug = BL_L, switch
lug = BH_A (and the same for the Normal channel). Dossier names: Bright Hi = Instrument 1, Bright Lo = Instrument 2,
Normal Hi = Mic 1, Normal Lo = Mic 2.

### First stage V1 (12AY7), volume and tone network

| Part | Printed | Between | Function |
|---|---|---|---|
| V1B / V1A | 12AY7 | plate V1B_P / V1A_P, grid V1B_G / V1A_G, both cathodes on K1 | Bright (upper) / Normal (lower) triode, joined by a cathode wire |
| 100K "Load Resistor" x2 | 100K | B+3 to V1B_P, B+3 to V1A_P | plate loads; B+3 enters at the dot between the two resistors |
| 820 "Cathode Resistor" | 820 | K1 to GND | shared cathode |
| "Bypass Cap" | 25-15 (as read) | K1 to GND | shared cathode bypass |
| "Coupling Cap" x2 | .1-400 | V1B_P to V1B_C, V1A_P to V1A_C | channel couplers |
| Volume Pot Bright | 1 MEG | track GND to MIX, wiper V1B_C | volume |
| Volume Pot Normal | 1 MEG | track MIX to GND, wiper V1A_C | volume |
| Tone Pot | 1 MEG | track TONE_T to TONE_B, wiper MIX | tone |
| "Bright Cap" | .0005 | V1B_C to TONE_T | Bright capacitor (500 pF) |
| "Tone Cap" | .005 | TONE_B to GND | tone capacitor (5 nF) |

### V2A (12AX7 gain stage), V2B (cathodyne)

| Part | Printed | Between | Function |
|---|---|---|---|
| V2A | 12AX7 | plate V2A_P, grid MIX, cathode V2A_K | gain stage |
| 100 K "Load Resistor" | 100 K | B+3 to V2A_P | plate load |
| 1500 "Cathode Resistor" | 1500 | V2A_K to GND | cathode |
| "Bypass Cap" | 25-25 | V2A_K to GND | cathode bypass |
| "Coupling Cap" | .02-400 | V2A_P to V2B_G | V2A to V2B coupler |
| V2B | 12AX7 | plate V2B_P, grid V2B_G, cathode V2B_K | phase inverter |
| 56 K "Load Resistor" | 56 K | B+3 to V2B_P | plate load |
| 1500 "Cathode Resistor" | 1500 | V2B_K to V2B_T | cathodyne cathode resistor |
| 56 K "Tail Load Resistor" | 56 K | V2B_T to GND | lower cathode load |
| 1 MEG "Grid Leak" | 1 MEG | V2B_G to V2B_T | bootstrapped grid return |

### Output stage (V3, V4 = 6V6GT)

| Part | Printed | Between | Function |
|---|---|---|---|
| "Coupling Cap" (inverted) | .1-400 | V2B_P to INV_G3 | to V3 |
| "Coupling Cap" (non-inverted) | .1-200 | V2B_K to NON_G4 | to V4 |
| "Grid Leak" x2 | 220K | INV_G3 to GND, NON_G4 to GND | grid return to ground |
| "Grid Stopper" x2 | 1500 | INV_G3 to V3_G, NON_G4 to V4_G | stoppers |
| V3 / V4 | 6V6GT | plate OT_PA / OT_PB, screen B+2, grid V3_G / V4_G, cathode K3 | push-pull pair (tetrode symbols) |
| "Cathode Resistor" | 250-5W | K3 to GND | shared bias resistor |
| "Bypass Cap" | 25-25 | K3 to GND | bypass |

### Output transformer and speaker

OT primary: top end OT_PA (V3 plate), centre tap B+1, bottom end OT_PB (V4 plate). OT secondary: upper end SPK_HOT,
lower end GND. Speaker jack and Extra jack tips on SPK_HOT, sleeves GND; internal speaker connected between SPK_HOT
and GND; extra jack (labelled "EXTRA 8-ohm SPKR.") empty. No winding data, impedances or turns ratio are printed.

### Supply

| Part | Printed | Between | Function |
|---|---|---|---|
| 5Y3GT V5 | 5Y3GT | plates RECT_PA, RECT_PB; DC lead B+1; other filament lead RECT_F | rectifier |
| PT HV winding | (CT label only) | RECT_PA to RECT_PB, centre tap GND | high-voltage secondary |
| PT "5V AC Rectifier Heater" | 5V | B+1 to RECT_F | rectifier filament winding |
| PT 6.3 V winding | arrows only | to "all 6.3 volt filaments and pilot lite" | heaters (not drawn) |
| Filter caps x3 | 16-450 | B+1, B+2, B+3 to GND | reservoir and filter caps |
| "Voltage Dropping Resistor" | 5000 | B+1 to B+2 | |
| "Dropping Resistor" | 22 K | B+2 to B+3 | |

B+ taps: OT centre tap (6V6 plates) = B+1; both 6V6 screens (direct wires, no screen resistors) = B+2; both V1 100k
loads, the V2A 100k load and the V2B 56K plate load = B+3.

### Mains

Power cord: narrow prong (black, hot) to AC SWITCH (single pole, drawn open) to transformer primary top; wide prong
(white, neutral) to 2 AMP FUSE (slow blow) to transformer primary bottom; green wire to chassis safety ground.

## 3. Input jack plug states

Only the Bright-Hi jack is drawn with a plug. The register encodes the other states from the switch-lug wiring
(A1 below). With "empty jack = switch lug closed to tip lug":

| Plugs | Resulting grid network | Check against the dossier |
|---|---|---|
| none | Hi node grounded by its switch lug, Lo node tied to it: both 68k to ground | silent |
| Hi only (the drawn state) | source on BH_A, Lo node tied to it: 68k in parallel = 34k; 1 MEG to ground | dossier Sec. 06: 34k; Sec. 02: 1 MOhm input |
| Lo only | Hi node grounded: source, 68k, grid, 68k, ground: -6.02 dB, 136k load | dossier Sec. 06 and 02: 136k, -6 dB |
| both | each source feeds the grid through its own 68k; 1 MEG stays on the Hi source node | not stated |

## 4. Ambiguities and how they were resolved

| ID | Where | Reading | Confidence |
|---|---|---|---|
| A1 | jack switch wires | thin arrow wire from the 1 MEG's ground end to the top line = Hi jack switch lug wired to ground; thick arrow wire from the Hi node to the Lo jack = Lo jack switch lug wired to the Hi node. Fig. 8 draws the same two wires as jack-lug connections. Normally-closed action is the standard switched-jack behaviour (DERIVED). | Hi-only state: high (drawn). Others: medium-high |
| A2 | black block at the Bright grid node | heavy junction mark; lower 68k joins the grid | high |
| A3 | B+3 riser crossing the V1A plate wire | crossing without a dot, no connection | high |
| A4 | Bright cap top | on the volume-pot wiper node, as dossier Sec. 08 says | high |
| A5 | V1 bypass cap label | reads "25-15"; 25 uF is certain, working voltage digit is not (Fig. 8 prints 25 V). Not an electrical parameter | capacitance high, rating about 70 % |
| A6 | .0005 / .005 units | uF, equal to 500 pF / 5 nF | high |
| A7 | speaker jacks | main load is the speaker between SPK_HOT and ground; both jacks sit on SPK_HOT; whether an Extra-jack plug disconnects the internal speaker is not drawn | topology medium-high; jack switching undeterminable |
| A8 | 6V6 grid leaks | return to ground, not to the cathode | high |
| A9 | 6V6 screens | direct to B+2 | high |
| A10 | rectifier filament | across the 5V winding, whose top end is tied to the DC output node | high |
| A11 | electrolytic polarity | no marks on the drawing; positive end is the non-ground node (DERIVED) | high |
| A12 | ground symbols | one chassis ground net | high |
| A13 | pot wiper arrows | arrowhead direction has no electrical meaning | high |

## 5. What the drawing does NOT contain

- heater wiring for V1-V4, heater centre-tap or hum-balance grounding, the pilot lamp (only "To ALL 6.3 VOLT FILAMENTS
  AND PILOT LITE" arrows);
- power-transformer winding voltages (only "5V AC Rectifier Heater"), turns, resistances, mains tap;
- output-transformer data: impedance, ratio, winding resistances, leakage, inductances, core;
- internal speaker value (the annotation says "Typical: 8 Ohms"; the extra jack is labelled 8 ohm); no T-S data;
- tube pin numbers, interelectrode capacitances, datasheet curves, DC operating voltages and currents;
- pot tapers and tolerances; resistor and capacitor tolerances, ESR, leakage, polarity marks;
- standby switch, bleeder, death cap, ground switch (Fig. 8 says the last two are deleted), global feedback, screen
  resistors, any grid/cathode protection other than the 1500 ohm stoppers;
- guitar source impedance and cable capacitance; stray and wiring capacitances;
- any modern PR 728 production revision.

## 6. Cross-check of the printed values against the dossier (Sec. 14 table, Sec. 02, 06-11 prose)

| ID | Item | Drawing | Dossier | Class |
|---|---|---|---|---|
| D1 | V2A to V2B coupling cap | .02-400 (Fig. 8: .02, 600 V) | 0.022 uF | **value conflict**; keep the dossier 0.022 uF as the default (specification, standard value), record 0.02 uF as the drawing alternative |
| D2 | V1 cathode bypass working voltage | 25-15 (other bypass caps 25-25; Fig. 8: 25 V) | 25 uF only | rating only |
| D3 | output coupling caps | .1-400 and .1-200 | 2 x 0.1 uF | rating only (asymmetric 400 V / 200 V on the drawing) |
| D4 | output grid stoppers | 2 x 1500 ohm | not in the Sec. 14 table (Sec. 14: grid/screen protection parts unresolved) | on drawing, not in table; keep, HISTORICAL |
| D5 | pots | 1 MEG each | 1 MOhm nominal | consistent |
| D6 | bright / tone caps | .0005 / .005 | 500 pF / 0.005 uF | consistent |
| D7 | typical output | 8 ohm, 18 W, 12 Vrms, 34 Vpp, 1.5 A | 12.5 W, 10 Vrms, 1.25 A, 28.28 Vpp | annotation differs (original 5E3 typical vs modern spec); use the dossier numbers |
| D8 | tube types | 6V6GT, 5Y3GT | 6V6 (manual: 6V6GC), 5Y3 | suffix only |
| D9 | input impedances | Hi alone 1 MEG, Lo alone 136k (derived) | 1 MOhm / 136 kOhm | consistent |
| D10 | fuse | 2 A slow blow, neutral lead | F2A L 250 V | consistent |
| D11 | supply | 3 x 16-450, 5000, 22K | 3 x 16 uF, 5 k, 22 k | consistent (450 V on the drawing only) |
| D12 | all other table rows | same numbers | same | consistent |

Of the above, D1 is the only electrical value conflict.

## 7. Comparison with the repository netlist (done after the register was committed)

The netlist was opened only after the register and this file were committed (8301ea6). The register's
`netlist_mapping` section declares the net-name and element-name mapping; `scripts/check_netlist_against_register.py`
(CTest `netlist_vs_schematic_register`) diffs the two graphs through that mapping and
`--self-test` (CTest `netlist_register_checker_selftest`) proves it fails on 14 deliberately broken netlists
(wrong value, moved grid leak, grounded bootstrap return, screens on the wrong rail, missing part, extra part on a
drawn net, wrong evidence tag, split cathodes, ungrounded OT secondary, ...), none of which is stored.

Result for `data/netlist/cd57_nominal.net` (94 elements):

| Category | Elements | Outcome |
|---|---|---|
| drawn resistors and capacitors | 22 R + 13 C | same nets, same values, except D1 below |
| pot tracks | 6 (two per pot) | wiper and ends as drawn (Bright volume: ground end / mix end; Normal volume: mix end / ground end; tone: ends to the 500 pF and 5 nF caps, wiper on the mix node); track resistances add to 1 MEG |
| tubes | V1B, V1A, V2A, V2B, V3, V4 | electrodes as drawn: V1 cathodes shared, V2A grid on the mix node, cathodyne grid return on the 1.5k/56k junction, 6V6 screens on B+2, plates on the OT primary ends, both 6V6 cathodes on one node |
| rectifier | D1, D2 | plates on the HV winding ends, cathodes on B+1 |
| output transformer | OT | primary ends on the 6V6 plates, centre tap on B+1, secondary hot to the speaker node, cold end to ground |
| HV winding | Rs_a, Rs_b | each half-winding from a rectifier plate to ground (centre tap) |
| jack switches | Sw_b1, Sw_b2, Sw_n1, Sw_n2 | Hi switch lug to ground, Lo switch lug to the Hi node; the engine (`AmpCircuit.h`) closes a switch when its jack is empty and opens it when plugged, which reproduces all four plug states of section 3 |
| supply | C1-C3, Rdrop1, Rdrop2 | B+1 / B+2 / B+3 as drawn; B+3 feeds both V1 loads, the V2A load and the cathodyne plate load |

Topology: **no discrepancy** between the independent transcription and the netlist. The LU kernel therefore did not
need regeneration, and `cd57_validate` is unchanged (27 passed, 0 failed; only the wall-clock time differs).

Values: **one** difference, D1 (`Cc_2`). The drawing prints .02-400; the dossier Sec. 14 table says 0.022 uF. The netlist
keeps the dossier value as the default (it is the specification given to this project; 0.022 uF is the standard value
and the drawing's .02 is the coarser number) and now records the drawing value as a documented alternative in a
netlist comment and in the register (`value_overrides`).

Waived (drawn but deliberately not in the audio-rate netlist, each with a reason in the register): AC switch (runtime
`power` control), fuse, mains cord and safety ground, power-transformer primary, 5 V rectifier-heater winding, 6.3 V
winding, speaker-jack contacts (the internal speaker is permanently connected).

Not on the schematic and present in the netlist, with the evidence tag the checker enforces:

| Netlist items | Tag required | Finding |
|---|---|---|
| source chain `Rsrc`, `Lpu`, `Ccab`, `Rsel_*`, `Rpt_*` (13) | PROP | correct |
| interelectrode capacitances `Cgk_*`, `Cgp_*`, `Cpk_*` (18) | DATASHEET | tag correct; the numbers are not in the dossier text (datasheet-class values), the notes now say so |
| speaker equivalent `Re`, `Le`, `Res`, `Cmes`, `Lces`; HV winding `Rs_a`, `Rs_b`; OT parameters | ASSUMED | correct |
| `Rext`, `Rdum` (extra speaker, bench dummy load) | PROP | correct |
| `tol=` attributes | n/a | specimen-variation widths, not drawing data; the netlist header now says PROP |

Label corrections made: the OT note said "FTDO-59 class ratio HIST", but that ratio comes from an aftermarket
specification (dossier Sec. 04: not OEM evidence), so the note now says so (the `ev=ASSUMED src=S10` tag was already
right); the jack-switch notes now state that the lug wiring is drawn while the normally-closed action is derived.
