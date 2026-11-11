// NAPI surface of libdmhost.so (native/napi_ohos.c).
export const start: (filesDir: string, lang: string) => number;
export const bootInfo: () => string;
export const surfaceInfo: () => string;
export const execProbe: () => string;
// validatePack returns "0:<reason>" on success or "-<code>:<reason>" on the first problem (P1 data pack rules).
export const validatePack: (dir: string) => string;
// freeSpace returns the bytes available on the volume holding the path, or -1 when it cannot be read.
export const freeSpace: (path: string) => number;

// Data pack ZIP import, reference-based: the .zip is read straight from the picker file's fd (no copy).
// zipInfo(fd) -> "rc:unpackedBytes:entries:reason" (rc 0 = OK).
export const zipInfo: (fd: number) => string;
// unzipStart(fd, outDir) -> "0" started, or "-<code>:reason". Runs on a background thread.
export const unzipStart: (fd: number, outDir: string) => string;
// unzipPoll() -> "state:rc:done:total:reason"; 0 idle, 1 running, 2 done, 3 failed. The terminal state is consumed.
export const unzipPoll: () => string;

// hapticsStart registers the ArkTS vibrator player: callback(intensity 0..1, sharpness 0..1, epoch).
export const hapticsStart: (callback: (intensity: number, sharpness: number, epoch: number) => void) => number;
export const hapticsStop: () => number;
export const hapticsSuspend: (suspended: boolean) => number;

// press maps an OHOS touch/button to the host pad (bit 0 = A).
export const press: (buttons: number, down: boolean) => number;

// touch forwards one finger of the ArkUI touch overlay: normalized [0,1) picture coordinates (top left), down
// adds or updates the finger slot, up removes it. The native side sends the whole set to dm_touch_set.
export const touch: (id: number, x: number, y: number, down: boolean) => number;
// touchClear lifts every forwarded finger.
export const touchClear: () => void;

// pointerFrame returns the menu adapter frame as JSON ("" when the adapter is absent).
export const pointerFrame: () => string;
// pointerEvent sends one menu pointer event: type 0 down, 1 move, 2 up, 3 cancel; x/y in 1920x1080 game pixels.
export const pointerEvent: (token: number, type: number, x: number, y: number) => number;
// touchHint: packed Touchable hint (layout | first << 8 | second << 16 | extra << 24).
export const touchHint: () => number;
// pointerControls: 0 show, 1 hide (loading), 2 keep (short scene gap).
export const pointerControls: () => number;
export const pointerClear: () => number;

// stick sets the left/right analog axes, each -32767..32767 (up/right positive).
export const stick: (lx: number, ly: number, rx: number, ry: number) => number;

// Debug: base64-log runtime captured shaders to hilog.
export const dumpCaptures: () => void;

// Debug: clear runtime captured shaders.
export const clearCaptures: () => void;
