# Encore FAS Frame Interval Detector

This document describes the detector algorithm in the Encore FAS kernel module.
The detector reads the timestamp of each frame and sends events to a user-space daemon.
The file encore_fas_det.h contains the primary source code.

## 1. Scope

1. Input data is the timestamp of each `Surface::queueBuffer` call in one game process.
2. Time values use ticks from the arm64 virtual counter (`CNTVCT_EL0`). All durations in this document use ticks. Event timestamps use `CLOCK_MONOTONIC` in nanoseconds.
3. The daemon configures 1 to 8 target frame rates. The daemon can also set the display vsync period and a lock flag.
4. The detector does not learn a baseline from the frame stream. The target list defines normal performance.
5. The detector sends events only. The daemon sets CPU and GPU frequency policies.
6. All operations use integer arithmetic.
7. One detector instance (a listener) handles one game process. The detector processes one frame stream per listener. Frames from multiple surfaces mix into this stream.
8. The vsync period remains constant until reconfigured.

## 2. Notation

### 2.1 Terms

| Term | Meaning |
| ----- | ----- |
| Interval | Time duration between two consecutive frames. |
| Target | Valid frame rate from the target list. |
| Active target | Currently selected target frame rate. |
| Period | Nominal frame interval for a target. |
| Slot | Required time period to process and queue one frame. |
| Hitch | Frame interval exceeding reference interval plus margin by at least one slot. |
| Missed slots | Number of frame slots lost during a hitch. |
| Deficit | Sustained average interval exceeding active period beyond tolerance limit. |
| Window | Measurement duration used to calculate average frame interval. |
| Tolerance | Accepted interval deficit limit (5% of target period). |
| Margin | Allowed threshold addition before classifying frame as a hitch. |
| Listener | Detector state context assigned to one game process. |

### 2.2 Symbols

| Symbol | Meaning | Unit |
| ----- | ----- | ----- |
| $F$ | Counter frequency | Hz |
| $t_i$ | Time of frame $i$ | ticks |
| $d_i$ | Interval, $t_i - t_{i-1}$ | ticks |
| $f_k$ | Rate of target $k$ | fps |
| $P_k$ | Period of target $k$ | ticks |
| $B_k$ | Half width of the band of target $k$ | ticks |
| $A$ | Index of the active target | none |
| $P$ | Period of the active target, $P_A$ | ticks |
| $V$ | Vsync period | ticks |
| $c$ | Moving average of the interval | ticks |
| $R$ | Reference interval | ticks |
| $H$ | Margin | ticks |
| $m_i$ | Missed slots of interval $i$ | count |
| $x_i$ | Normalized excess of interval $i$ | Q16 |
| $S$ | CUSUM value | Q16 |
| $s$ | Decaying scale of the interval deviation $\lvert d - c \rvert$ | ticks, Q16 |
| $K$ | Margin multiplier of $s$, 3 | ratio |
| $\epsilon$ | Tolerance, 0.05 | ratio |
| $h$ | CUSUM alarm limit, 1.0 | ratio |
| $W$ | Window length | ticks |
| $n$ | Number of intervals in a window | count |
| $\mu_w$ | Mean interval of a window | ticks |
| $\rho_w$ | $\mu_w / P$ | Q16 |
| $T_{soft}$, $T_{hard}$, $T_{pause}$ | Watchdog delays | ticks |

Q16 represents a fixed-point number with 16 fractional bits. The value 1.0 equals 65536.

## 3. Configuration

### 3.1 Inputs

| Input | Meaning |
| ----- | ----- |
| `fps[]`, `count` | Array and count of target frame rates. |
| `vsync_ns` | Display vsync period in nanoseconds (0 selects default value). |
| `FAS_CFG_LOCK_DOWN` | Flag to disable switching to lower frame rates. |

### 3.2 Validation

The module returns `-EINVAL` and rejects configuration if any condition is met:

1. `count` equals 0 or exceeds 8.
2. Frame rate equals 0 or exceeds 1000 fps.
3. `flags` contains bits other than `FAS_CFG_LOCK_DOWN`.
4. `vsync_ns` is non-zero and tick period is less than $F / 1000$ or greater than $F / 10$.
5. Sorted adjacent periods differ by less than 12%:

$$
100 P_{k+1} < 112 P_k
$$

A rejected configuration does not change detector state.

### 3.3 Derived Settings

The detector sorts frame rates from fastest to slowest. Target 0 is the fastest target. For each target $k$:

$$
P_k = \left\lfloor \frac{F + \lfloor f_k / 2 \rfloor}{f_k} \right\rfloor
\qquad
B_k = \left\lfloor \frac{5 P_k}{100} \right\rfloor
$$

The detector calculates initial parameters:

| Value | Definition |
| ----- | ----- |
| $V$ | Specified vsync period, or $P_0$ if omitted. |
| $W_{min}$ | Minimum window duration $F / 4$ (250 ms). |
| $T_{pause}$ | Pause threshold $\max(F, 10 P_{n-1})$ where $P_{n-1}$ is slowest period. |
| Active target | Default target 0 until acquisition completes. |
| $W$ | Target window length $\max(W_{min}, 8P)$, adjusted to active target. |

Target band $k$ is $[P_k - B_k, P_k + B_k]$. Target bands do not overlap because $P_{k+1} / P_k \ge 1.12$ and overlap requires ratio $\le 1.105$.

### 3.4 Target List Purpose

Operation at any listed target rate indicates normal performance. Single-rate applications use a list with one entry. Multi-rate lists allow the detector to adapt across performance tiers.

The lockdown flag prevents switching to lower target rates. When locked, frame rate drops below the active target set the degraded state.

### 3.5 Detector Reset

New configuration calls and initial registration reset the detector state:

1. Clear all state fields to 0.
2. Set `acquiring` flag to 1.
3. Set `pending` target index to none (`FAS_NONE`).
4. Set watchdog stage to `IDLE`.

New configurations preserve the event sequence counter `seq`.

## 4. State

| Variable | Meaning |
| ----- | ----- |
| `last` | Timestamp of previous frame. |
| `have_last` | Set to 1 when initial frame is received. |
| `acquiring` | Set to 1 while selecting initial target rate. |
| `win_start`, `win_n` | Start timestamp and interval count of current window. |
| `c_q4` | Moving average interval scaled by 16 ($16c$). |
| `scale_q` | Decaying deviation scale $s$ in Q16 format. |
| `S` | CUSUM deficit metric in Q16 format. |
| `degraded` | Set to 1 when frame rate is below active target. |
| `paused` | Set to 1 when watchdog detects frame pause. |
| `wd_stage` | Watchdog stage (`ARMED`, `SOFT_SENT`, `HARD_SENT`, or `IDLE`). |
| `ok_windows` | Count of consecutive normal windows. |
| `pending`, `pending_n` | Candidate target index and matching window count. |
| `seq` | Listener event sequence counter. |

## 5. Structure

### 5.1 Pipeline

```
frame time t_i
    |
    v
interval d_i = t_i - t_(i-1)
    |
    +--> gap check (d_i >= T_pause) ------> resync, RESUMED
    |
    +--> hitch detector (each interval) --> SMALL_JANK, BIG_JANK
    |
    +--> deficit detector (CUSUM) --------> DEGRADED
    |
    +--> window estimator (each window) --> RECOVERED, RATE_SWITCH

watchdog timer (restarted at each frame) --> BOOST_SOFT, BOOST_HARD, PAUSED
```

### 5.2 State Machine

```
                 configuration
                       |
                       v
                +-------------+
                |  ACQUIRING  |
                +-------------+
                       | first window closes (RATE_SWITCH)
                       v
  +-------------+  CUSUM alarm (DEGRADED)   +-------------+
  |   HEALTHY   |-------------------------->|  DEGRADED   |
  |             |<--------------------------|             |
  +-------------+  2 clean windows          +-------------+
        |          (RECOVERED)                     |
        |          or rate switch                  |
        |                                          |
        +----- no frame for T_pause (PAUSED) ------+
                          |
                          v
                     +---------+
                     | PAUSED  |--- next frame (RESUMED) ---> HEALTHY
                     +---------+
```

The `PAUSED` state clears the `DEGRADED` state. If frame rate remains low after resuming, the CUSUM triggers a new `DEGRADED` alarm.

## 6. Frame Procedure

The detector executes this procedure for each frame timestamp $t$. The function returns watchdog timer delay ticks. A return value of 0 leaves the timer unchanged.

1. If `have_last` equals 0, set `have_last` and `acquiring` to 1. Set `last` and `win_start` to $t$. Set `win_n` to 0. Return 0.
2. Calculate $d = t - \texttt{last}$. If $d \le 0$, discard frame and return 0. (Handles out-of-order counter reads across CPUs).
3. Set `last` to $t$.
4. If `paused` equals 1 or $d \ge T_{pause}$, execute resync procedure (Section 11). Return 0 if `acquiring` equals 1, otherwise return $T_{soft}$.
5. If `acquiring` equals 1:
   1. Increment `win_n`.
   2. If $t - \texttt{win\_start} < W_{min}$, return 0.
   3. Execute acquisition procedure (Section 7). Return $T_{soft}$.
6. Set `FAS_EVF_WATCHDOG` event flag if `wd_stage` is not `ARMED`. Set `wd_stage` to `ARMED`.
7. Calculate $R$ and $H$ (Section 8).
8. If $d \ge R + H$, execute hitch procedure (Section 9). Otherwise update $c$ and $s$ (Section 8.3).
9. If `degraded` equals 0, add $d$ to CUSUM (Section 10). If CUSUM reaches alarm threshold, emit `DEGRADED`, set `degraded` to 1, and set $S$ to 0.
10. Increment `win_n`. If $t - \texttt{win\_start} \ge W$, execute window procedure (Section 12).
11. Return $T_{soft}$ based on updated state parameters.

Single frame evaluations output at most 4 events. The detector drops extra events exceeding 4.

## 7. Acquisition

Acquisition selects the initial active target using one frame measurement window.

1. Initial frame starts measurement window without calculating interval (Section 6, step 1).
2. Subsequent frames record frame intervals $n$.
3. When $t - \texttt{win\_start} \ge W_{min}$, calculate mean interval $\mu = \lfloor (t - \texttt{win\_start}) / n \rfloor$.
4. If $\mu$ falls inside target band $k$, select target $k$.
5. Otherwise select target with largest period $\le \mu$. If all target periods exceed $\mu$, select target 0.
6. Set `acquiring` to 0 and `wd_stage` to `ARMED`.
7. Execute rate switch procedure (Section 12.3) to emit `RATE_SWITCH`.
8. Set `win_start` to `last` and `win_n` to 0.

Step 5 handles frame rates between targets by selecting the faster rate target. CUSUM tracking reports subsequent performance deficits as `DEGRADED`.

Watchdog timer events are suppressed while `acquiring` equals 1.

## 8. Reference Interval and Margin

### 8.1 Reference Interval

$$
R = \min\bigl(\max(P, c),\ 2P\bigr)
$$

$R$ tracks recent frame intervals. Values are clamped between $P$ and $2P$. Deficit detection uses $P$ directly, so increases in $R$ do not mask frame deficits.

### 8.2 Margin

$$
H = \min\bigl(\max(V / 2,\ K s),\ \max(V / 2,\ 0.75 R),\ R\bigr)
$$

1. Minimum threshold $V / 2$ represents half a vsync frame duration.
2. Parameter $s$ (Section 8.4) is a decaying scale of the frame deviation. $K = 3$ (`FAS_SCALE_MULT`).
3. Maximum cap $0.75R$ keeps the hitch detection threshold below $1.75R$, so a single missed slot (interval $2P$) is always reported. When $V / 2$ is larger than $0.75R$, the cap is $V / 2$. In all cases $H \le R$.

There is no warmup period. Setup initializes $s$ so that $K s = V / 2$, and $s$ adapts from the first normal frame.

### 8.3 Update

The detector updates cadence $c$ and scale $s$ using normal frames only. It calculates deviation using $c$ prior to updating:

$$
dev = \lvert d - c \rvert
\qquad
c \leftarrow c + \frac{d - c}{8}
$$

Initial parameters: Target switches and resync reset $c = P$. Setup initializes $s = V / (2K)$. Target switches and resync preserve $s$.

### 8.4 Decaying Scale

Scale $s$ moves by a fixed part of its own value on each normal frame:

$$
s \leftarrow
\begin{cases}
s + \max(s \cdot 2^{-3},\ 1) & dev > s \\
s - \max(s \cdot 2^{-7},\ 1) & dev \le s
\end{cases}
$$

The result has a lower limit of $1/16$ tick. The shifts are `FAS_SCALE_UP_SHIFT = 3` and `FAS_SCALE_DOWN_SHIFT = 7`.

**Equilibrium.** The up and down steps are equal in expectation when $P(dev > s) \cdot 2^{-3} = P(dev \le s) \cdot 2^{-7}$. This gives $P(dev > s) = 1/17$, so $s$ follows the 0.94 quantile of $dev$ for any interval distribution. Equal shifts would give the median. In simulation, a median scale did not reach the same false hitch rate on heavy-tailed jitter even with $K = 14$, and it raised the margin on moderate jitter.

**Bounded influence.** A step is a fixed part of $s$. One large interval changes $s$ by at most $2^{-3}$ (12.5%), regardless of its size. Frames that cause a hitch event do not update $s$.

**Decay.** In quiet periods $s$ falls by a factor of $e$ in about 128 frames, which is 2 seconds at 60 fps. After a noisy period the margin returns to its floor within a few seconds.

**Trade-offs.**

1. The false hitch rate depends on the jitter distribution. The scale follows the bulk of the noise. On heavy-tailed jitter (10% of frames late by an exponential with a mean of 8 ms), the detector reports about 70 false hitches per minute at 60 fps. A hitch rate signal is the correct way to handle this case.
2. The margin changes within seconds. If noise comes in short bursts, the margin falls between bursts and the detector reports the jank inside the bursts.

Scale $s$ persists across rate switches and resync calls.

## 9. Hitch Detector

Frame intervals meeting $d \ge R + H$ trigger hitch events. Missed slot count $m$:

$$
m = \left\lfloor \frac{d - H}{R} \right\rfloor \qquad (m \ge 1)
$$

| Missed slots | Event | Rule |
| ----- | ----- | ----- |
| $1 \le m \le 2$ | `SMALL_JANK` | Suppressed while `degraded` equals 1. |
| $m \ge 3$ | `BIG_JANK` | Always generated. |

Hitch event records contain interval duration $d$, slot loss count $\min(m, 65535)$, and watchdog flags.

### 9.1 Threshold Reference

Values below assume minimum noise margin $H = V / 2$. Large hitch threshold equals $3R + H$.

| Target | $V$ | $H$ | Hitch threshold ($d \ge$) | Large hitch threshold ($d \ge$) |
| ----- | ----- | ----- | ----- | ----- |
| 30 fps | 16.67 ms | 8.33 ms | 41.7 ms | 108.3 ms |
| 60 fps | 16.67 ms | 8.33 ms | 25.0 ms | 58.3 ms |
| 60 fps | 8.33 ms | 4.17 ms | 20.8 ms | 54.2 ms |
| 120 fps | 8.33 ms | 4.17 ms | 12.5 ms | 29.2 ms |
| 144 fps | 6.94 ms | 3.47 ms | 10.4 ms | 24.3 ms |

## 10. Deficit Detector

The deficit detector uses a one-sided CUSUM algorithm. CUSUM tracking executes while `degraded` equals 0.

$$
x_i = \frac{\min(d_i,\ 1.5P) - P}{P} \quad \text{(Q16)}
$$

$$
S_i = \max(0,\ S_{i-1} + x_i - \epsilon)
$$

The detector triggers an alarm when $S_i \ge h$. Upon alarm, it emits `DEGRADED`, sets `degraded` to 1, and resets $S$ to 0. The window estimator clears `degraded` state (Section 12.2).

| Parameter | Function |
| ----- | ----- |
| $\epsilon = 0.05$ | Frame delay excess below tolerance limit does not accumulate. |
| $h = 1.0$ | Alarm requires cumulative deficit of one frame slot above tolerance. |
| Upper clip $1.5P$ | Single intervals add at most $0.5 - \epsilon = 0.45$ to $S$. Prevents 1 or 2 isolated hitches from alarming. |
| Lower bound $-1.0$ | Interval lower limit based on $d > 0$. Single intervals reduce $S$ by at most $1.0 + \epsilon$. |

### 10.1 Deficit Pressure

Listener pressure equals $\min(S, 1.0)$ in Q16 format. Value 65536 indicates imminent deficit alarm. Pressure remains 0 while `degraded` equals 1.

### 10.2 Detection Properties

For sustained interval excess $\delta = (d - P) / P$, required frame count to alarm $N_{det}$:

$$
N_{det} = \left\lceil \frac{h}{\min(\delta, 0.5) - \epsilon} \right\rceil
$$

Detection duration equals $N_{det} P (1 + \delta)$. Continuous excess $\le \epsilon$ never triggers an alarm.

Burst hitches with subsequent catch-up frames cancel out in CUSUM tracking. Permanent frame shifts accumulate continuously towards deficit alarms.

Timestamp jitter does not cause CUSUM metric drift. For timestamp $t_i = n_i + e_i$ with zero-mean independent jitter $e_i$, interval $d_i = P + e_i - e_{i-1}$. Summing $L$ intervals yields $LP + e_n - e_{n-L}$, depending only on boundary jitter values.

## 11. Pause and Resume

Frame intervals $\ge T_{pause}$ trigger pause handling instead of hitch classification. The initial frame following a pause executes the resync procedure:

1. If `paused` equals 1, emit `RESUMED`.
2. Discard current frame interval.
3. Clear `paused`, `degraded`, `ok_windows`, `S`, and `pending_n` to 0. Set `pending` to none (`FAS_NONE`).
4. Set `win_start` to $t$ and reset `win_n` to 0.
5. If `acquiring` equals 0, set `wd_stage` to `ARMED` and reset $c = P$.

Pause intervals generate no hitch events. Delayed timers missing pause events still execute resync without emitting `RESUMED`.

## 12. Window Estimator

### 12.1 Definition

Measurement windows start at frame timestamp $t_s$ and close at first frame timestamp $t_e$ meeting $t_e - t_s \ge W$. The subsequent window starts at timestamp $t_e$.

$$
\mu_w = \left\lfloor \frac{t_e - t_s}{n} \right\rfloor
\qquad
\rho_w = \left\lfloor \frac{(t_e - t_s) \cdot 65536}{n P} \right\rfloor
$$

Windows incorporate all frame intervals, including hitches. Interval summation $t_e - t_s$ cancels catch-up frames in mean interval $\mu_w$. Jitter error in $\mu_w$ equals $\sqrt{2}\,\sigma_t / n$ for timestamp jitter standard deviation $\sigma_t$.

Upon window closure, the detector evaluates recovery, executes target rate tracking, and opens a new window.

### 12.2 Performance Recovery

The detector executes recovery checks while `degraded` equals 1:

1. If $\rho_w \le 1 + \epsilon$, increment `ok_windows`. Otherwise set `ok_windows` to 0.
2. When `ok_windows` reaches 2, clear `degraded`, `ok_windows`, and $S$ to 0. Emit `RECOVERED` event containing $\mu_w$.

Recovery threshold matches entry tolerance $\epsilon$. Deficits exceeding $\epsilon$ trigger and maintain `DEGRADED` status.

### 12.3 Target Rate Tracking

1. Search target candidates from fastest to slowest to find target $k$ matching mean interval $\mu_w$ within its band.
2. If no target band matches $\mu_w$, or if $k$ equals active target $A$, reset `pending` to none (`FAS_NONE`), set `pending_n` to 0, and exit.
3. If $k \ne$ `pending`, update `pending` to $k$ and reset `pending_n` to 0.
4. Increment `pending_n` (capped at 255).
5. If $P_k > P$ (slower target rate) and lockdown flag is active, exit.
6. Set required window count $need = 4$ for lower target rates, or $need = 2$ for higher target rates.
7. If `pending_n` $\ge need$, execute rate switch procedure.

**Target Switch Procedure.** Switching active target to index $k$:

1. Update active target index to $k$. Recalculate $P$, target reciprocal, and window length $W$.
2. Reset $c = P$.
3. Clear `pending` to none (`FAS_NONE`). Reset `pending_n`, `degraded`, `ok_windows`, and $S$ to 0.
4. Emit `RATE_SWITCH` event. The `fps` field specifies the new target frame rate.

Transitioning to lower frame rate targets requires additional matching windows to verify rendering stability.

## 13. Watchdog Timer

The watchdog timer detects frame delays prior to frame arrival. Each frame evaluation restarts the timer.

### 13.1 Delay Calculations

$$
T_{soft} = R + H
\qquad
T_{hard} = 3R + H
\qquad
T_{pause} = \max(F,\ 10 P_{n-1})
$$

Delays measure elapsed time since previous frame. Parameters $R$ and $H$ reflect state at execution time.

### 13.2 Timer State Machine

Timer callbacks evaluate elapsed time $e = \max(0, now - \texttt{last})$ based on `wd_stage`:

| Stage | Condition | Action |
| ----- | ----- | ----- |
| `ARMED` | $e < T_{soft}$ | No action. Return 0. |
| `ARMED` | $e \ge T_{soft}$ | Set `SOFT_SENT`. Emit `BOOST_SOFT`. Return $\max(T_{hard} - e, 1)$. |
| `SOFT_SENT` | $e < T_{hard}$ | Return $T_{hard} - e$. |
| `SOFT_SENT` | $e \ge T_{hard}$ | Set `HARD_SENT`. Emit `BOOST_HARD`. Return $\max(T_{pause} - e, 1)$. |
| `HARD_SENT` | $e < T_{pause}$ | Return $T_{pause} - e$. |
| `HARD_SENT` | $e \ge T_{pause}$ | Set `IDLE` and `paused`. Emit `PAUSED`. Enter idle poll state. |
| `IDLE` | Any | Enter idle poll state. |

**Idle Polling.** The callback sets idle status and returns delay $8 W_{min}$ (2 s). Caller verifies target process status. Dead processes trigger listener removal. Active processes re-arm watchdog timers.

Registration and reconfiguration procedures arm the timer using idle poll delay.

### 13.3 Event Deduplication

Frame stalls persisting past $T_{soft}$ generate watchdog and subsequent hitch events. Hitch events set `FAS_EVF_WATCHDOG` when `wd_stage` equals `SOFT_SENT` or `HARD_SENT`. Daemons filter flagged events to avoid duplicate stall counts.

## 14. Events

### 14.1 Event Definitions

| Event | Source | Condition | `frametime` field | Daemon action |
| ----- | ----- | ----- | ----- | ----- |
| `BOOST_SOFT` | Watchdog | Frame delay $\ge R + H$ | Duration since previous frame | Apply minor boost |
| `BOOST_HARD` | Watchdog | Frame delay $\ge 3R + H$ | Duration since previous frame | Apply major boost |
| `PAUSED` | Watchdog | Frame delay $\ge T_{pause}$ | Duration since previous frame | Clear performance boosts |
| `SMALL_JANK` | Frame | $1 \le m \le 2$ | Frame interval $d$ | Log or apply minor boost |
| `BIG_JANK` | Frame | $m \ge 3$ | Frame interval $d$ | Apply major boost |
| `DEGRADED` | Frame | CUSUM deficit alarm | Frame interval $d$ | Increase base performance level |
| `RECOVERED` | Window | 2 consecutive normal windows | Mean interval $\mu_w$ | Lower base performance level |
| `RESUMED` | Frame | Initial frame following `PAUSED` | 0 | Reset daemon state |
| `RATE_SWITCH` | Acquisition, Window | Active target rate changed | 0 | Update daemon model |

### 14.2 Event Payload

Event timestamps convert timer ticks to nanoseconds upon queue insertion.

| Field | Content |
| ----- | ----- |
| `timestamp_ns` | `CLOCK_MONOTONIC` timestamp at event queuing. |
| `frametime_ns` | Event-specific duration value. |
| `fps` | Active target rate at event generation. |
| `missed` | Missed slot count $\min(m, 65535)$ for hitches, 0 otherwise. |
| `flags` | Bit 0 indicates `FAS_EVF_WATCHDOG`. |
| `pressure_q16` | Deficit pressure metric (Section 10.1). |
| `seq` | Monotonic listener event sequence counter. |

### 14.3 Delivery Queue

1. Listeners share a global event queue holding 512 entries.
2. Queue overflows discard the oldest event and increment the drop counter.
3. Sequence gaps in `seq` indicate lost events. Daemons query `FAS_IOC_GET_STATE` to read `fps`, flags (`ACQUIRING`, `DEGRADED`, `PAUSED`), `pressure_q16`, `seq`, drop counts, and the current $R$, $H$, and $V$ in nanoseconds (`ref_ns`, `margin_ns`, `vsync_ns`). The soft watchdog fires when no frame arrives for `ref_ns + margin_ns`. `vsync_ns` is the effective value, which is the period of the fastest target if the configured vsync is 0.
4. `read()` returns up to 8 events per call. User buffer size must accommodate at least one 48-byte event structure; smaller buffers return `-EINVAL`.

### 14.4 Event Volume

Normal rendering produces no events. Maximum event rates per stream:

| Source | Maximum Rate |
| ----- | ----- |
| Hitch | 1 per frame (`BIG_JANK` only while `degraded` equals 1). |
| Window | 2 per window (`RECOVERED`, `RATE_SWITCH`). |
| Watchdog | 3 per stall sequence. |

## 15. Integer Arithmetic

1. 64-bit integer divisions use `div_u64()`, `div64_u64()`, and `div64_s64()`.
2. Target reciprocals $\lfloor 2^{48} / P \rfloor$ precalculate for active targets. CUSUM excess $x$:

$$
x = \frac{\bigl(\min(d, 1.5P) - P\bigr) \cdot \lfloor 2^{48} / P \rfloor}{2^{32}}
$$

3. Moving average calculations use signed 64-bit arithmetic right shifts:

```c
c_q4 += ((d << 4) - c_q4) >> 3;
```

4. Arithmetic right shifts preserve sign bits for negative values (guaranteed by Linux kernel).
5. Evaluated intervals stay below $T_{pause}$. For $F \le 4$ GHz and 1 fps target, $T_{pause} = 4 \times 10^{10}$ ticks. 16-bit shifts prevent 64-bit integer overflow. CUSUM products remain $\le 2^{48}$.
6. Tick-to-nanosecond conversions use fixed multiplier and shift values, capping input durations at 60 s.
7. System counter frequency $F$ reads `CNTFRQ_EL0`. Out-of-range or inaccurate values fall back to measured hardware frequencies rounded up to 10 kHz.
8. Scale $s$ uses Q16 format matching CUSUM metrics. Each step uses a right shift of $s$, with a minimum of 1. The product $K s$ stays within 64 bits since $s \le d_{max} \cdot 2^{16}$ and $d_{max} < T_{pause}$.

## 16. Concurrency Model

1. Each listener maintains a raw spinlock. Detector state access requires `raw_spin_lock_irqsave()`.
2. Global mutex serializes listener table operations. Code acquires global mutex prior to listener locks.
3. **Frame Handler.** Filters calls from non-matching thread group IDs. Acquires lock, executes frame procedure, queues events, and schedules watchdog timer. Releases lock and wakes waiting readers.
4. **Timer Callback.** Acquires lock, executes timer procedure, queues events, and schedules next timer stage. Releases lock and wakes readers. Callback does not use automatic timer restart.
5. **Configuration Update.** Builds new detector instance on stack. Acquires lock, copies state, preserves `seq`, and arms idle polling timer.
6. Watchdog timers specify $1/16$ delay slack.

## 17. Parameter Reference

| Parameter | Default | Unit | Effect of Larger Value |
| ----- | ----- | ----- | ----- |
| `FAS_TOL_PCT` ($\epsilon$) | 5 | % of $P$ | Reduces deficit alarms. Widens target rate bands. Slows recovery. |
| `FAS_MIN_RATIO_PCT` | 112 | % | Enforces larger separation between target frame rates. |
| `FAS_SCALE_MULT` ($K$) | 3 | ratio | Increases the margin for a given scale. Reduces false hitches. Reduces detection of small hitches. |
| `FAS_SCALE_UP_SHIFT` | 3 | shift | Reduces the growth step of $s$. Slows the response to new noise. Lowers the tracked quantile. |
| `FAS_SCALE_DOWN_SHIFT` | 7 | shift | Reduces the decay step of $s$. Slows the return to a low margin. Raises the tracked quantile. |
| `FAS_MARGIN_CAP_SHIFT` | 2 | shift | Raises the cap of $H$ toward $R$. A missed slot can sit on the threshold. |
| `FAS_MISS_BIG` | 3 | slots | Reduces `BIG_JANK` events. Delays `BOOST_HARD` watchdog triggers. |
| CUSUM upper clip | 0.5 | $P$ | Accelerates deficit alarms from large single hitches. |
| CUSUM alarm limit $h$ | 1.0 | $P$ | Delays deficit alarm triggers. |
| Minimum window $W_{min}$ | $F/4$ | ticks | Increases mean interval accuracy. Delays window decisions. |
| `FAS_WIN_PERIODS` | 8 | $P$ | Extends window durations for low target frame rates. |
| `FAS_OK_WINDOWS` | 2 | windows | Delays performance recovery transition. |
| `FAS_UP_WINDOWS` | 2 | windows | Delays rate switch transitions to higher target rates. |
| `FAS_DOWN_WINDOWS` | 4 | windows | Delays rate switch transitions to lower target rates. |
| Pause floor | $F$ | ticks | Delays `PAUSED` state detection. |
| `FAS_PAUSE_PERIODS` | 10 | $P_{n-1}$ | Delays `PAUSED` state detection for low target rates. |
| `FAS_IDLE_POLL_WINS` | 8 | $W_{min}$ | Slows detection of terminated target processes. |
| Cadence EWMA weight | 1/8 | ratio | Accelerates cadence adaptation $R$. |
| Reference cap | 2 | $P$ | Increases hitch detection thresholds in degraded states. |
| `FAS_OUT_MAX` | 4 | events | Increases maximum event capacity per evaluation call. |

## 18. False Alarm Analysis

### 18.1 False Hitch Rate

For Gaussian timestamp jitter with standard deviation $\sigma_t$, the interval deviation is $\sigma_d = \sqrt{2}\,\sigma_t$. The scale follows the 0.94 quantile of $\lvert d - c \rvert$, which is $1.88\sigma_d$, so $H = 3s \approx 5.6\sigma_d$ and the false hitch probability is:

$$
p = Q\!\left(\frac{H}{\sigma_d}\right) \approx 10^{-8}
$$

In practice the floor $V / 2$ is larger than $H$ for $\sigma_t$ below about 1 ms at 60 fps, and the floor sets the rate.

Non-Gaussian rendering jitter (thermal throttling, scheduler contention, IPC stalls) has heavier tails. The scale does not follow the tail, so the false hitch rate rises with tail weight. Simulations at 60 fps with a 0.4 ms Gaussian base:

| Tail | False hitches per minute |
| ----- | ----- |
| None | 0 |
| 3% of frames, exponential mean 4 ms | 13 |
| 10% of frames, exponential mean 8 ms | 69 |

These values match the previous quantile tracker. The margin does not stay high after a noisy period, so single-slot hitches are reported again within seconds.

### 18.2 False Deficit Alarm Rate

CUSUM deficit alarm rates rely on Gaussian jitter assumptions. False alarm probability at lag $L$ requires $e_n - e_{n-L} \ge (h + L\epsilon) P$. Per-frame probability bound:

$$
\sum_{L \ge 1} Q\!\left(\frac{(h + L\epsilon) P}{\sqrt{2}\,\sigma_t}\right)
$$

Dominant term $L = 1$ remains small for jitter standard deviations $\sigma_t \le 0.15 P$.

This bound is a conservative sanity check, not a distributional guarantee. Unlike the hitch margin (Section 8.4), tolerance $\epsilon$ and alarm limit $h$ are fixed rather than adapted to the observed noise distribution, so actual false alarm rates under heavy-tailed jitter may differ from the Gaussian estimate above. The fixed tolerance is deliberate: deficit alarms test sustained interval excess against the fixed period $P$, which is a specification, not an estimated noise level, so there is no distribution to adapt to in the same sense as the hitch margin.
