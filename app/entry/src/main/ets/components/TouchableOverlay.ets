// Touchable-style touch controls for the HarmonyOS host.
//
// This is a port of the touch layer in Touchable-main (internal/platform/ios/App.m, MIT, Copyright (c) mzhao):
//   · large, mostly transparent touch areas instead of a crowded button pad;
//   · a left D-pad that picks one of eight directions from the finger's angle (a diagonal holds two buttons);
//   · a large right A area, a B button, and the +/− buttons of the rhythm layout;
//   · a visible touch effect: the pressed arrow lights, the A area glows at the finger, and a fading ring follows
//     the finger in the menu touch layer;
//   · menu mode forwards the finger set to the native touchscreen (dm_touch_set), the direct-touch path that
//     Touchable's menus use, instead of only synthesizing controller buttons.
//
// The native side (native/napi_ohos.c) exposes press(mask, down) and touch(id, x, y, down). Coordinates are
// normalized [0,1) on the 16:9 output picture, origin top left, as in Touchable's ss_pointer events.
import dmhost from 'libdmhost.so';

// The menu adapter publishes a frame of 1920x1080 targets; the ArkTS layer only needs mode and token because
// the core does the hit test itself. This mirrors the JSON from dmhost.pointerFrame().
interface PointerTargetJson {
  id: number;
  x: number;
  y: number;
  w: number;
  h: number;
  f: number;
  b: number;
}
interface PointerFrameJson {
  mode: number;
  token: number;
  tutorial: number;
  count: number;
  targets: PointerTargetJson[];
}

// Switch NpadButton masks (same bits as hle_hid.c / Touchable ss_input.c).
const BTN_A: number = 1 << 0;
const BTN_B: number = 1 << 1;
const BTN_X: number = 1 << 2;
const BTN_Y: number = 1 << 3;
const BTN_ZL: number = 1 << 8;
const BTN_ZR: number = 1 << 9;
const BTN_PLUS: number = 1 << 10;
const BTN_MINUS: number = 1 << 11;
const DIR_LEFT: number = 1 << 12;
const DIR_UP: number = 1 << 13;
const DIR_RIGHT: number = 1 << 14;
const DIR_DOWN: number = 1 << 15;

function pressMask(mask: number, down: boolean): void {
  try {
    dmhost.press(mask, down);
  } catch (e) {
  }
}

// ss_touch.h: SS_TOUCH_DPAD=0, ONE=1, TWO=2, MENU=3, PAD=4; extra bits SHOW_DPAD=1, SHOW_X=2, SHOW_Y=4.
function buttonMask(n: number): number {
  if (n === 0) return BTN_A;
  if (n === 1) return BTN_B;
  if (n === 2) return BTN_X;
  if (n === 3) return BTN_Y;
  if (n === 12) return DIR_LEFT;
  if (n === 13) return DIR_UP;
  if (n === 14) return DIR_RIGHT;
  if (n === 15) return DIR_DOWN;
  return 0;
}
function buttonLabel(n: number): string {
  if (n === 0) return 'A';
  if (n === 1) return 'B';
  if (n === 2) return 'X';
  if (n === 3) return 'Y';
  if (n === 12) return 'L';
  if (n === 13) return 'U';
  if (n === 14) return 'R';
  if (n === 15) return 'D';
  return '';
}

function clamp01(v: number): number {
  return v < 0 ? 0 : v > 0.999 ? 0.999 : v;
}

// A single glass button. The press look is the same white light Touchable draws inside its glass: the fill
// brightens and the border grows with the finger down. It stays pressed while any finger is in it.
@Component
export struct TouchBtn {
  @Prop label: string = '';
  @Prop bit: number = 0;
  @Prop btnSize: number = 52;
  @Prop round: boolean = true;
  @State active: number = 0;

  aboutToDisappear(): void {
    if (this.active > 0) {
      this.active = 0;
      pressMask(this.bit, false);
    }
  }

  build() {
    Stack() {
      Text(this.label)
        .fontSize(this.label.length > 1 ? 15 : 22)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.active > 0 ? '#FFFFFFFF' : '#B3FFFFFF')
        .textAlign(TextAlign.Center)
    }
    .width(this.btnSize)
    .height(this.btnSize)
    .borderRadius(this.round ? this.btnSize / 2 : 18)
    .backgroundColor(this.active > 0 ? '#33FFFFFF' : '#14000000')
    .border({ width: 1, color: this.active > 0 ? '#8CFFFFFF' : '#38FFFFFF' })
    .shadow({ radius: this.active > 0 ? 18 : 6, color: this.active > 0 ? '#66FFFFFF' : '#33000000', offsetY: 2 })
    .onTouch((event: TouchEvent) => {
      if (event.type === TouchType.Down) {
        const was: number = this.active;
        this.active = this.active + 1;
        if (was === 0) {
          pressMask(this.bit, true);
        }
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        if (this.active > 0) {
          this.active = this.active - 1;
          if (this.active === 0) {
            pressMask(this.bit, false);
          }
        }
      }
    })
  }
}

// The left D-pad. One finger selects one of eight directions from its angle; a diagonal holds two directions.
// It keeps the touch outside the drawn circle until the finger lifts (Touchable's PadDpad does the same).
@Component
export struct DpadZone {
  @State dirs: number = 0;
  @State glowX: number = -1;
  @State glowY: number = -1;
  private held: number = 0;

  private setDirs(next: number): void {
    const changed: number = next ^ this.held;
    if (changed & 1) {
      pressMask(DIR_LEFT, (next & 1) !== 0);
    }
    if (changed & 2) {
      pressMask(DIR_UP, (next & 2) !== 0);
    }
    if (changed & 4) {
      pressMask(DIR_RIGHT, (next & 4) !== 0);
    }
    if (changed & 8) {
      pressMask(DIR_DOWN, (next & 8) !== 0);
    }
    this.held = next;
    this.dirs = next;
  }

  aboutToDisappear(): void {
    this.setDirs(0);
  }

  @Builder
  arm(bit: number, x: number, y: number, angle: number) {
    Stack() {
      Circle()
        .width(36)
        .height(36)
        .fill((this.dirs & bit) !== 0 ? '#38FFFFFF' : '#00000000')
      Polygon({ width: 18, height: 18 })
        .points([[9, 1], [17, 17], [1, 17]])
        .fill((this.dirs & bit) !== 0 ? '#FFFFFFFF' : '#B3FFFFFF')
        .rotate({ angle: angle })
    }
    .width(36)
    .height(36)
    .position({ x: x, y: y })
  }

  build() {
    Stack() {
      // Touchable puts a 35% dark layer behind its glass controls; without it the white D-pad is
      // invisible on the bright menu backgrounds.
      Circle()
        .width(132)
        .height(132)
        .fill('#59000000')
        .stroke('#40FFFFFF')
        .strokeWidth(1)
      Circle()
        .width(10)
        .height(10)
        .fill('#66FFFFFF')
        .position({ x: 61, y: 61 })
      if (this.glowX >= 0) {
        Circle()
          .width(70)
          .height(70)
          .fill('#1FFFFFFF')
          .position({ x: this.glowX - 35, y: this.glowY - 35 })
      }
      this.arm(2, 48, 5, 0)
      this.arm(8, 48, 91, 180)
      this.arm(1, 5, 48, 270)
      this.arm(4, 91, 48, 90)
    }
    .width(132)
    .height(132)
    .onTouch((event: TouchEvent) => {
      if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        this.setDirs(0);
        this.glowX = -1;
        this.glowY = -1;
        return;
      }
      let touches: TouchObject[] = event.touches;
      if (touches.length === 0) {
        touches = event.changedTouches;
      }
      if (touches.length === 0) {
        return;
      }
      const x: number = touches[0].x;
      const y: number = touches[0].y;
      this.glowX = x;
      this.glowY = y;
      const dx: number = x - 66;
      const dy: number = y - 66;
      if (Math.sqrt(dx * dx + dy * dy) < 20) {
        this.setDirs(0);
        return;
      }
      const ax: number = Math.abs(dx);
      const ay: number = Math.abs(dy);
      let next: number = 0;
      if (ax > ay * 0.4142) {
        next |= dx < 0 ? 1 : 4;
      }
      if (ay > ax * 0.4142) {
        next |= dy < 0 ? 2 : 8;
      }
      this.setDirs(next);
    })
  }
}

// The right A area: Touchable's large transparent zone. The finger's glow follows it; the letter says which
// button the area holds. Any number of fingers can hold A (both halves of a split multiplayer screen can).
@Component
export struct AZone {
  @Prop bit: number = BTN_A;
  @Prop label: string = 'A';
  @Prop zoneW: number = 150;
  @Prop zoneH: number = 220;
  @Prop labelSize: number = 26;
  @Prop hideLabel: boolean = false;
  @State active: number = 0;
  @State glowX: number = -1;
  @State glowY: number = -1;

  aboutToDisappear(): void {
    if (this.active > 0) {
      this.active = 0;
      pressMask(this.bit, false);
    }
  }

  build() {
    Stack() {
      if (this.active > 0 && this.glowX >= 0) {
        Circle()
          .width(110)
          .height(110)
          .fill('#1FFFFFFF')
          .position({ x: this.glowX - 55, y: this.glowY - 55 })
      }
      if (!this.hideLabel) {
        Text(this.label)
          .fontSize(this.labelSize)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.active > 0 ? '#E6FFFFFF' : '#40FFFFFF')
      } else if (this.active > 0 && this.glowX >= 0) {
        // Touchable's TouchFeedbackView caption: only while a finger is down.
        Text(this.label)
          .fontSize(24)
          .fontWeight(FontWeight.Medium)
          .fontColor('#E6FFFFFF')
          .position({ x: Math.max(0, this.glowX - 14), y: Math.max(0, this.glowY - 48) })
      }
    }
    .width(this.zoneW)
    .height(this.zoneH)
    .onTouch((event: TouchEvent) => {
      const touches: TouchObject[] = event.touches;
      if (event.type === TouchType.Down) {
        const was: number = this.active;
        this.active = this.active + 1;
        if (was === 0) {
          pressMask(this.bit, true);
        }
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        if (this.active > 0) {
          this.active = this.active - 1;
          if (this.active === 0) {
            pressMask(this.bit, false);
          }
        }
        if (this.active === 0) {
          this.glowX = -1;
          this.glowY = -1;
        }
      }
      if (touches.length > 0) {
        this.glowX = touches[0].x;
        this.glowY = touches[0].y;
      }
    })
  }
}

// The full overlay. Menu mode forwards every finger that is not on a control to the native touchscreen, and
// paints a ring at the finger like Touchable's TouchFeedbackView. The controls themselves still synthesize Npad
// buttons, so the same layout works for rhythm levels and for menus.
@Component
export struct TouchableOverlay {
  @State winW: number = 0;
  @State winH: number = 0;
  @State pointerMode: number = 0;     // 1 menu, 2 map; 0 = no direct-menu frame
  @State pointerToken: number = 0;
  @State pointerX: number = -1;
  @State pointerY: number = -1;
  @State hintLayout: number = 3;      // default SS_TOUCH_MENU
  @State hintFirst: number = 0;
  @State hintSecond: number = 0;
  @State hintExtra: number = 0;
  @State loading: boolean = false;
  @State inLevel: boolean = false;
  @State tutorialButtons: number = 0;   // Touchable's tutorial_buttons: Y is shown only while this is non-zero
  private pointerTouchId: number = -1;
  private pointerTimer: number = -1;

  aboutToAppear(): void {
    this.pointerTimer = setInterval(() => {
      this.pollPointer();
    }, 33);
  }

  aboutToDisappear(): void {
    if (this.pointerTimer >= 0) {
      clearInterval(this.pointerTimer);
      this.pointerTimer = -1;
    }
  }

  // Poll the adapter frame. The core owns hit testing, so only mode and token are needed here.
  private pollPointer(): void {
    try {
      const hint: number = dmhost.touchHint();
      const layout: number = hint % 256;
      const first: number = Math.floor(hint / 256) % 256;
      const second: number = Math.floor(hint / 65536) % 256;
      const extra: number = Math.floor(hint / 16777216) % 256;
      if (this.hintLayout !== layout) this.hintLayout = layout;
      if (this.hintFirst !== first) this.hintFirst = first;
      if (this.hintSecond !== second) this.hintSecond = second;
      if (this.hintExtra !== extra) this.hintExtra = extra;
    } catch (e) {
    }
    // Touchable's ss_pointer_controls: 1 = WAIT (hide), 2 = short frame gap (keep the controls as
    // they are), 0 = normal. Without KEEP the direct-menu pad flashes back for a frame on every
    // screen transition.
    let controls: number = 0;
    try {
      controls = dmhost.pointerControls();
    } catch (e) {
    }
    if (controls === 1) {
      this.loading = true;
      return;
    }
    if (controls !== 2) {
      this.loading = false;
    }
    let text: string = '';
    try {
      text = dmhost.pointerFrame();
    } catch (e) {
      return;
    }
    if (!text || text.length < 2) {
      if (controls === 2) {
        return;   // short gap between screens: keep the current controls
      }
      if (this.pointerMode !== 0) {
        this.pointerMode = 0;
        this.pointerToken = 0;
        this.inLevel = false;
      }
      this.tutorialButtons = 0;
      return;
    }
    try {
      const frame = JSON.parse(text) as PointerFrameJson;
      if (this.tutorialButtons !== frame.tutorial) this.tutorialButtons = frame.tutorial;
      if (this.pointerToken !== frame.token) {
        this.pointerToken = frame.token;
        this.pointerTouchId = -1;
        this.pointerX = -1;
        this.pointerY = -1;
      }
      const mode: number = frame.mode === 1 || frame.mode === 2 ? frame.mode : 0;
      if (this.pointerMode !== mode) {
        this.pointerMode = mode;
      }
      // Touchable's inLevel: a HINTS frame inside a level whose hint is not MENU. Outside a level the
      // adapter may still hold the last level hint; force MENU so menus get their D-pad back.
      const level: boolean = frame.mode === 3 && this.hintLayout !== 3;
      if (this.inLevel !== level) {
        this.inLevel = level;
      }
    } catch (e) {
    }
  }

  // Screen vp -> 1920x1080 game coordinates (the picture is a 16:9 fit inside the window).
  private toGame(x: number, y: number): number[] {
    if (this.winW <= 0 || this.winH <= 0) {
      return [clamp01(x / 1430) * 1920, clamp01(y / 636) * 1080];
    }
    let picW: number = this.winW;
    let picH: number = picW * 9 / 16;
    if (picH > this.winH) {
      picH = this.winH;
      picW = picH * 16 / 9;
    }
    const bx: number = (this.winW - picW) / 2;
    const by: number = (this.winH - picH) / 2;
    return [clamp01((x - bx) / picW) * 1920, clamp01((y - by) / picH) * 1080];
  }

  private handlePointer(event: TouchEvent): void {
    if (this.pointerMode === 0 || this.pointerToken === 0) {
      return;
    }
    const changed: TouchObject[] = event.changedTouches;
    for (let i = 0; i < changed.length; i++) {
      const t: TouchObject = changed[i];
      const g: number[] = this.toGame(t.x, t.y);
      if (event.type === TouchType.Down) {
        if (this.pointerTouchId < 0) {
          this.pointerTouchId = t.id;
          this.pointerX = t.x;
          this.pointerY = t.y;
          try {
            dmhost.pointerEvent(this.pointerToken, 0, g[0], g[1]);
          } catch (e) {
          }
        }
      } else if (event.type === TouchType.Move) {
        if (t.id !== this.pointerTouchId) {
          continue;
        }
        this.pointerX = t.x;
        this.pointerY = t.y;
        try {
          dmhost.pointerEvent(this.pointerToken, 1, g[0], g[1]);
        } catch (e) {
        }
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        if (t.id !== this.pointerTouchId) {
          continue;
        }
        try {
          dmhost.pointerEvent(this.pointerToken, event.type === TouchType.Up ? 2 : 3, g[0], g[1]);
        } catch (e) {
        }
        this.pointerTouchId = -1;
        this.pointerX = -1;
        this.pointerY = -1;
      }
    }
  }

  private padVisible(): boolean {
    return this.pointerMode === 0 && !this.loading;
  }

  // Touchable only applies a level's ONE/TWO/DPAD/PAD hint inside a level. In menus (or when no level
  // frame is current) it uses SS_TOUCH_MENU, even if the last level hint is still stored.
  private effectiveLayout(): number {
    return this.inLevel ? this.hintLayout : 3;
  }

  private showDpad(): boolean {
    // Rhythm Heaven should not show the D-pad outside a level. Menus use the direct touch layer
    // (or the right A area); only an actual level with a DPAD/PAD hint shows the D-pad.
    if (!this.inLevel) {
      return false;
    }
    const layout: number = this.effectiveLayout();
    return this.padVisible() && (layout === 0 || layout === 4 || (this.hintExtra & 1) !== 0);
  }

  private showOne(): boolean {
    return this.padVisible() && this.effectiveLayout() === 1;
  }

  private showTwo(): boolean {
    return this.padVisible() && this.effectiveLayout() === 2;
  }

  private showX(): boolean {
    return this.padVisible() && ((this.hintExtra & 2) !== 0 || this.effectiveLayout() === 4);
  }

  private showY(): boolean {
    if (!this.padVisible()) return false;
    // BeatSpell / full controller keep Y always; a normal level shows it only during the tutorial phase.
    if ((this.hintExtra & 4) !== 0 || this.effectiveLayout() === 4) return true;
    return this.inLevel && this.tutorialButtons !== 0;
  }

  private showShoulders(): boolean {
    return this.padVisible() && this.effectiveLayout() === 4;
  }

  // Touchable rhythm geometry: zones cover the left/right half of the 16:9 picture; B and +/− sit in the
  // letterbox bars. The D-pad keeps its existing ring style and position.
  private barWidth(): number {
    let picW: number = this.winW;
    let picH: number = picW * 9 / 16;
    if (picH > this.winH) {
      picH = this.winH;
      picW = picH * 16 / 9;
    }
    return Math.max(0, (this.winW - picW) / 2);
  }

  private contentLeft(): number {
    return this.barWidth() + 4;
  }

  private contentRight(): number {
    return this.winW - this.barWidth() - 4;
  }

  private contentTop(): number {
    return 60;
  }

  private contentBottom(): number {
    return Math.max(0, this.winH - 6);
  }

  private halfX(): number {
    return (this.contentLeft() + this.contentRight()) / 2;
  }

  private twoWidth(): number {
    return Math.max(0, (this.halfX() - this.contentLeft()) / 2);
  }

  build() {
    Stack() {
      // Touchable rhythm layout. The hint from the core selects the left area:
      //   MENU/DPAD/PAD -> small D-pad, ONE -> one large key area, TWO -> two side-by-side areas.
      // The right A area and B/+/- stay in every layout; X/Y/ZL/ZR appear for BeatSpell / Drum Lessons.
      if (this.padVisible()) {
        TouchBtn({ label: 'B', bit: BTN_B, btnSize: 44 })
          .position({ x: Math.max(0, this.barWidth() / 2 - 22), y: 28 })
        if (this.showY()) {
          // Touchable shows Y only while the game's tutorial hint reports a demo button.
          TouchBtn({ label: 'Y', bit: BTN_Y, btnSize: 44 })
            .position({ x: Math.max(0, this.barWidth() / 2 - 22), y: 80 })
        }
        TouchBtn({ label: '+', bit: BTN_PLUS, btnSize: 42 })
          .position({ x: Math.max(0, this.winW - this.barWidth() / 2 - 21), y: 28 })
        TouchBtn({ label: '−', bit: BTN_MINUS, btnSize: 42 })
          .position({ x: Math.max(0, this.winW - this.barWidth() / 2 - 21), y: 84 })
      }
      if (this.showDpad()) {
        DpadZone()
          .position({ x: 154, y: Math.max(0, this.winH - 146) })
      }
      if (this.padVisible()) {
        AZone({
          hideLabel: true,
          zoneW: Math.max(0, this.contentRight() - this.halfX() - 4),
          zoneH: Math.max(0, this.contentBottom() - this.contentTop())
        })
          .position({ x: this.halfX() + 4, y: this.contentTop() })
      }

      if (this.showOne()) {
        // Touchable SS_TOUCH_ONE: the whole left half of the picture is one transparent touch area.
        AZone({
          bit: buttonMask(this.hintFirst),
          label: buttonLabel(this.hintFirst),
          zoneW: Math.max(0, this.halfX() - this.contentLeft()),
          zoneH: Math.max(0, this.contentBottom() - this.contentTop()),
          labelSize: 40,
          hideLabel: true
        })
          .position({ x: this.contentLeft(), y: this.contentTop() })
      }
      if (this.showTwo()) {
        // Touchable SS_TOUCH_TWO: the left half splits into its own left/right areas.
        AZone({
          bit: buttonMask(this.hintFirst),
          label: buttonLabel(this.hintFirst),
          zoneW: this.twoWidth(),
          zoneH: Math.max(0, this.contentBottom() - this.contentTop()),
          labelSize: 34,
          hideLabel: true
        })
          .position({ x: this.contentLeft(), y: this.contentTop() })
        AZone({
          bit: buttonMask(this.hintSecond),
          label: buttonLabel(this.hintSecond),
          zoneW: this.twoWidth(),
          zoneH: Math.max(0, this.contentBottom() - this.contentTop()),
          labelSize: 34,
          hideLabel: true
        })
          .position({ x: this.contentLeft() + this.twoWidth(), y: this.contentTop() })
      }
      if (this.showX()) {
        TouchBtn({ label: 'X', bit: BTN_X, btnSize: 42 })
          .position({ x: Math.max(0, this.winW - 240), y: Math.max(0, this.winH / 2 - 168) })
      }
      if (this.showShoulders()) {
        TouchBtn({ label: 'ZL', bit: BTN_ZL, btnSize: 46 })
          .position({ x: Math.max(0, this.winW - 290), y: Math.max(0, this.winH / 2 + 132) })
        TouchBtn({ label: 'ZR', bit: BTN_ZR, btnSize: 46 })
          .position({ x: Math.max(0, this.winW - 238), y: Math.max(0, this.winH / 2 + 132) })
      }

      // Direct menu: a full-screen transparent layer over the picture. The core has the target frame and does
      // the hit test, so the UI only forwards the finger in 1920x1080 coordinates.
      if (this.pointerMode !== 0) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor('#00000000')
          .onTouch((event: TouchEvent) => {
            this.handlePointer(event);
          })
        if (this.pointerX >= 0) {
          Stack() {
            Circle()
              .width(76)
              .height(76)
              .fill('#12FFFFFF')
              .stroke('#66FFFFFF')
              .strokeWidth(1)
            Circle()
              .width(12)
              .height(12)
              .fill('#CCFFFFFF')
          }
          .width(76)
          .height(76)
          .position({ x: this.pointerX - 38, y: this.pointerY - 38 })
          .hitTestBehavior(HitTestMode.None)
        }
        // Touchable keeps a Back button over a touch menu; direct tapping replaces the rest of the pad.
        TouchBtn({ label: 'B', bit: BTN_B, btnSize: 44 })
          .position({ x: Math.max(0, this.barWidth() / 2 - 22), y: 28 })
      }

    }
    .width('100%')
    .height('100%')
    .onAreaChange((_old: Area, now: Area) => {
      this.winW = Number(now.width);
      this.winH = Number(now.height);
    })
  }
}
