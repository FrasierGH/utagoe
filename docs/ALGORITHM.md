# How Utagoe Rip works

Notes from reverse engineering `utagoe3-en_us.exe` (Utagoe Rip 3.0, Borland
C++Builder 2007, build date 2009-07-01, SHA-256
`307ff466774e13fda9a0560f351cd9eaa0f6fbde18def96bfe1a2134fa908052`).

The original Japanese release, `utagoe.exe` (SHA-256
`e242ab578ce3e2cb997f796b12314997b7fb23c59a4918da670d4c238d377b7c`), has
byte-identical code and data sections. The en_US build only differs in its
resources: translated form captions, the font (ＭＳ Ｐゴシック → Tahoma), a few
controls moved to fit the English text, a taller About box for the credits,
translated VCL string tables, and the main icon stored under a different
resource ID.
Addresses are virtual addresses in that binary (image base `0x400000`).

The binary exports the C++Builder unit initialisers, which gives the source
layout:

| Unit        | Classes           | Contents |
|-------------|-------------------|----------|
| `Mainform`  | `TForm1`, `CFFT`, `CFIR` | GUI, Ooura FFT, Kaiser FIR |
| `Cntrfocus` | `CenterFocus`     | stereo centre extraction |
| `Thvofft`   | `ThVocalFFT`      | per-channel FFT worker thread (the actual vocal extraction) |
| `Twave`     | `TWaveData`       | `mmio*` WAV reader/writer (16-bit PCM only) |
| `Vfunc`     | `TVocalFunc`      | block buffers, alignment/level searches, time-domain subtraction |
| `Vocalmain` | `TVocalMain`      | analysis and the main processing loop |
| `Setform`   | `TSetForm1`       | settings dialog, `UtagoeRip.ini` |

Floating point is x87; Borland's `__ftol` (`0x4b8650`) **truncates** toward zero,
so every float-to-int conversion in the program is a truncation.

## Inputs

Two 16-bit PCM WAV files with identical sample rate and channel count: the
*original* song and the *instrumental* (karaoke / off-vocal) version. The
instrumental may start earlier or later than the original, may be phase
inverted, and may drift slightly over time. With no instrumental the program
just runs the original through the post-processing chain (`0x411818`).

## The extraction (`ThVocalFFT`, `0x40c8b8`..`0x40d65c`)

Each channel is processed by a worker thread with a streaming STFT:

* `N = 8192`, hop `N/8 = 1024`, Hann window for analysis and synthesis
  (`0x40ca90`).
* Complex FFT of the windowed original `O` and instrumental `I` (Ooura's
  `cdft`, single precision).
* For bins `k = 0 .. N/2-1` (`0x40ce14`):

  ```
  g    = min(level, cap)
  V[k] = O[k] - g * I[k]
  if |I[k]| * cap > |O[k]|  and  phasediff(O[k], I[k]) < thr[k]:
      V[k] = 0
  ```

  `phasediff` is the absolute phase difference folded into `[0, pi]`.
  `thr[k] = kvol * max(0.15, pi * (k+1) / (N/2))`, a phase tolerance that grows
  with frequency, since a small time misalignment rotates high-frequency bins
  further. `cap = min(kvol, 1.5)`. `kvol` is the "Extractable Level" setting.

  With **Accuracy priority = Extraction** a second routine is used
  (`0x40d280`): `cap = kvol` and the phase test is dropped.
* The upper half of the spectrum is zeroed, the inverse FFT is taken and its real
  part is overlap-added with gain `5.33333 / 8`. That is `2` for the one-sided
  spectrum, divided by the Hann² overlap sum `8 * 3/8`.
* The ring buffers start with `N - hop` zeros, and the first `hop` samples of each
  synthesised frame are discarded. So the output lags the input by
  `N - hop = 7168` samples. The original never compensates this.

So this is not a sample-domain phase inversion. The instrumental is subtracted
bin by bin, and any bin that the instrumental explains (louder, and in phase) is
removed outright. That removal is what lets it cope with small timing and level
mismatches that would leave a waveform null full of residue.

## Block processing (`TVocalMain::0x410cfc`)

The file is processed in blocks of `2 * BlkSize` (default 0.2 s). The
instrumental window is 1.5 blocks. Blocks advance by half a block and only the
middle half of each block, `[blk/4, 3*blk/4)`, is used.

For each block:

1. Read the original block at `p_o` and the instrumental window starting at
   `p_i - range`. The instrumental is multiplied by -1 when its phase is inverted.
2. **Alignment** (`0x40eaf0`): try offsets `range ± d` (`d = 0, 1, ..., range`,
   centre-out, first minimum wins) minimising

   ```
   sum |r[i]| + |r[0]| + sum |r[i-1] - r[i]|,   r[i] = orig[i] - inst[ofs + i]
   ```

   summed over channels (L1 of the residual plus its total variation).
   *Processing mode* picks the signals compared: normal = L and R; L/R
   difference = `L-R` (cancels the centred vocal, so the instrumental drives
   the match); mono = `L+R`.
3. **Level**: fixed, or with *adaptive* level a coarse search (`1.0 ± 0.2`, step
   0.02) then a fine one (`± 0.03`, step 0.002) of `sum |trunc(orig - g*inst)|`
   (`0x40f008`).
4. **Subtraction**: the frequency method feeds the middle half of the block
   (original plus aligned instrumental) to the FFT threads (`0x40f5a4`). The
   waveform method subtracts in the time domain:
   `trunc(orig - g * inst[ofs+i])` (`0x40f3a0`). Mono mode uses `(L+R)/2` for
   both channels.
   With oversampling (waveform only), the instrumental is linearly interpolated
   `xN`. The offset is searched coarse (step `N`) then fine (`± N-1`).
5. Post-processing of the middle half (see below), then write it.
6. `p_i += found + base + blk/2`, `p_o += blk/2`.

Before the loop, `range` samples (or `range - ofs` if the original has the
longer lead-in) are processed with an empty instrumental, which passes the
original through. Then a quarter block is processed at level 0.95, so the
written stream is continuous (`0x41211c`).

## Automatic analysis (`0x40fdc4`)

Runs unless intro, time-shift, level and phase are all set manually.

1. **Initial offset and phase** (`0x4102c4`). For phase = normal, then inverted,
   try three estimates. Each is scored by a 30 s *trial run* (waveform
   subtraction at level 1.0, range 20, measuring mean `|residual|` vs mean
   `|original|`). Accept the first with residual/original < 0.8, otherwise
   keep the best.
   * *simple* (`0x411a7c`): first sample where `L+R >= 128` in each file, then
     an alignment search of ±0.25 s around that point (comparing 0.2 s of
     `L+R`).
   * *detailed* (`0x411cd4`): 20 ms mean-abs envelopes (60 s of the original,
     120 s of the instrumental) of `L-R` or `L+R`, L1-matched over ±30 s.
     Refined by an alignment search of ±40 ms on `L+R`.
2. **Drift**: a 120 s trial with range 20. `base` = truncated mean per-block
   offset, `spread` = `|mean - base| + mean |deviation| + 0.5`.
3. **Range**: trials with range `spread .. spread+8`. Stop when the residual
   changes by less than ±0.3 % between successive good ranges, otherwise take
   the best.
4. **Level**: a full-length trial with a coarse+fine level search every 31st
   block. The mean is the level used by the frequency method and by "Automatic
   (Averaged)".

The debug log lines (`range:%d vol:%f voc:%f org:%f ofs:%f bnsn:%f`, ...) come
from these trials.

## Post-processing

Applied to the output stream in this order:

* **Extraction centralization** (`CenterFocus`, `0x402a80`/`0x402d44`), stereo
  only. Same STFT framework (8192/1024, same 7168-sample delay). Per bin:
  * the louder channel's magnitude is pulled toward the quieter one:
    `|X| -> |Y| + p*(|X|-|Y|)`, with `p = max(0, 0.4 - 0.1*s)`;
  * if the L/R phase difference exceeds `thr[k] = (0.5*pi*(k+1)/(N/2) + 1) / s`,
    both channels are scaled by `0.5 + 0.5*cos(2*s*(diff - thr))`, or by 0 once
    the cosine argument reaches pi.

  `s = 0.5 + 0.25 * slider`.
* **Low-pass filter**: Kaiser-window FIR, pass edge `f`, stop edge `1.2 f`,
  80 dB (`0x404f5c`). `f = 2 kHz + 0.8 kHz * slider` (2–18 kHz).
* **High-pass filter**: the same design mirrored around Nyquist. Stop edge
  `0.75 f`, pass edge `f`, 60 dB, and the output is negated.
  `f = trunc((slider+1)^2 * 1.5 + 49)` Hz. Filter order is capped at 1000 taps,
  so low cut-offs get a wider transition than requested.
* **Soft clipping** (`0x40e728`): beyond ±31129 the excess is halved, then hard
  clipped to 16 bits.

## Settings (`TSetForm1`, `0x409c4c`)

| INI key (`[V30_Option]`) | GUI | Default | Conversion |
|---|---|---|---|
| `ProcMode` | Processing Mode: Normal / L/R Difference / Mono | 0 | |
| `MergeMode` | Extraction Method: By Frequency / By Waveform | 0 | |
| `SoundQty` | Accuracy Priority: Quality / Extraction | 0 | |
| `KvolPos` | Extractable Level slider | 6 | `<10: 0.6+0.1p`, else `1.5+0.3(p-9)` (0.6..4.8) |
| `LevelAdpt` | Instrumental Level: Auto (Averaged) / Auto (Adaptive) / Manual / None | 0 | |
| `KlvlPos` | manual level slider | 10 | `0.7 + 0.03p` |
| `IntroMode` | Intro Analysis: Automatic / Normal / Detailed / None | 0 | |
| `AdptMode`, `AdptNum` | Time Shift Correction: Automatic / Manual, Range | 0, 3 | range = `trunc(BlkSize*0.01*AdptNum)` samples |
| `KrkPhase` | Instrumental Phase: Automatic / Positive / Inverted | 0 | |
| `BlkSize` | Block Length (ms) | 100 | min 50; blocks are `2*BlkSize` |
| `OvspFlg`, `OvspMx` | Oversampling (waveform method) | off, 32 | |
| `CntrFlg`, `CntrPos` | Extraction Centralization | off, 6 | `s = 0.5 + 0.25p` |
| `LPFFlg`, `LPFPos` | Low Pass Filter | off, 10 | `2000 + 1000p` Hz |
| `HPFFlg`, `HPFPos` | High Pass Filter | off, 10 | `trunc(1.5(p+1)^2 + 49)` Hz |
| `KnameFlg` | Search for instrumental file | on | |
| `VnameFlg`, `VnameTxt` | Auto-name output | on, `_vo` | |

Mono input files force Processing Mode = Normal and disable centralization.
In the frequency method the level is the analysed one (0.95 when analysis is
skipped). The Instrumental Level setting only applies to the waveform method.

## Main window (`TForm1`, unit `Mainform`)

* **Loading a file** (`0x406fd4`): checks the RIFF/WAVE header and shows
  `%.3fkHz %d-bit Mono|Stereo`. Errors: the file can't be opened; not a WAVE
  file or damaged; not PCM or more than two channels. The 16-bit check only
  happens at Start.
* **Instrumental search** (`0x4058a8`, `0x406820`), run when the original is
  chosen:
  1. Up to two leading digits of the original's name (a track number) become
     `?` wildcards.
  2. Every `<name>*.wav` in the same folder is a candidate, except the original
     itself and the would-be output.
  3. The first keyword match (`inst`, `karaoke`, `off vocal`, `インスト`,
     `カラオケ`, ...) wins. Ties go to the closest file size; with no keyword
     match, the closest file size overall wins.
  4. If nothing is found, the name is shortened one character at a time and
     the search repeats. The output name is then built from the shortened name.

  The output name is `<name><suffix>.wav`. If the suffix starts with one of
  ` _-([`, that character is treated as a separator.
* **Drag and drop** (`0x406d58`): the drop point picks the box. Rows 8–89 go to
  the original, 90–168 to the instrumental, 169–264 to the output (for x in
  8–456). Only `.wav` files are accepted.
* **Enter** in the original box (and, by a quirk, in the output box) reloads
  the original and searches again. Enter in the instrumental box reloads it.
* **Start** (`0x407718`):
  1. Checks that all three names are given, that the output differs from
     both inputs, that sample rate and channel count match, and that both
     inputs are 16-bit.
  2. Asks before overwriting.
  3. Disables everything except Start (now "Abort") and Quit.
  4. Runs the processing.

  The same file in both input boxes selects original-only processing.
* **Progress**: refreshed every 12 blocks as `NN %` in the status box and
  `NN%-歌声りっぷ` as the application title. While the analysis runs, the
  status box shows `自動解析中` / `初期処理中` followed by an arrow that steps
  right once a second (`0x412494`).
* **Quit or close while processing** asks whether to abort.
* **Hidden debug mode**: double-clicking `DbgPanel` (8×8 px at 9,256) toggles
  it. The analysis log goes to `<original>.txt`, with
  `<date time>  N秒経過` (seconds elapsed) lines at the start, after the
  analysis, and at the end.
* **Help**: `ShellExecute("open", <exe dir>\UtagoeHelp.pdf)`.

## Playback (`TPlayForm1`)

A `TMediaPlayer` (MCI, millisecond time format) with Play, Pause, Stop and Prev
buttons, plus a trackbar spanning the file length. The trackbar has ticks
every 1/10 of the length, page size 1/40 and line size 1/100. A 400 ms timer
moves the trackbar. Moving the trackbar seeks, and resumes playback if it was
playing. Pause toggles between pause and resume.

## About (`TAboutForm`)

The logo is copied from an off-screen bitmap (logo at x = 150, 500 px of
padding) by a 50 ms timer:

1. Wait 6 ticks.
2. Scroll the source window in steps of 80 px from -106 until it reaches 375,
   so the logo flies through.
3. Wait 11 ticks.
4. Scroll in steps of 64 px from -106 to 150, so the logo lands.
5. For 16 ticks, stretch-copy a source rectangle of width 225 + d, with d
   taken from
   `150, 225, 244, 225, 145, 65, 15, -12, -20, -10, 8, 0, -4, 0, 2, 0`.

The last step is a damped squash-and-stretch bounce.

## Window layout (VCL)

All four DFMs say `PixelsPerInch = 96` and `TextHeight = 12`, the height of
the Japanese build's font, ＭＳ Ｐゴシック at `Height = -12`. When DjLizard made
the en_US build, they switched the main, Settings and Playback forms to Tahoma
`-12` but left `TextHeight` alone. Tahoma `-12` measures 14 pixels, so VCL's
`TCustomForm.ReadState` scales those forms by 14/12 when it loads them.

* Every `Left`/`Top` becomes `MulDiv(v, 14, 12)`. Widths and heights are
  taken from the scaled right and bottom edges.
* Coordinates are relative to the parent control, so the children of a
  `TGroupBox` round differently from controls placed directly on the form.
* Controls with `ParentFont = False` scale their font too. The Start button
  goes from 9 pt to 11 pt.

The About form kept ＭＳ Ｐゴシック in both builds and is not scaled. The
rebuild applies the same rule (scale by the form font's height / 12), so the
en_US windows come out at the original's 664×343 and 590×393 client size.

Other VCL behaviour the rebuild reproduces:

| Control | Behaviour |
|---|---|
| `TStaticText` | AutoSize = text extent + 4 pixels each way |
| `TEdit` | AutoSize height = `tmHeight + 8`, no margins |
| `TTrackBar` | always `TBS_FIXEDLENGTH \| TBS_ENABLESELRANGE` (the wide channel) |
| `TGroupBox` | draws its own themed frame: caption at x = 8, frame top at half the caption height |
| `TPageControl` | sheets start 2 px below `TCM_ADJUSTRECT`'s display rectangle, and use the themed tab body |
| `TUpDown` | sits flush against the right edge of its `Associate` edit, at the edit's height |
| `TMediaPlayer` | not themed; 29-pixel buttons sharing their frames, drawn by `DrawButtonFace(bsNew)` |
| Focus rectangles | always shown, not only after keyboard use |

The original is an ANSI program. On non-Japanese Windows its Japanese strings
show as `????`, and the en_US DFMs' UTF-8 captions are converted to the ANSI
code page. That is why the About box shows `TODAKEN`, not the DFM's
full-width `ＴＯＤＡＫＥＮ`.
