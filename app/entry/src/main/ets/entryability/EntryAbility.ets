// nx2android HarmonyOS NEXT entry ability: loads pages/Index, which owns the XComponent and the NAPI host.
// Scaffolded from the DevEco Studio stage-model template (as ohos-feasibility/probe); structure follows
// d2r-mobile-main/dm/harmony/app/entry/src/main/ets/entryability/EntryAbility.ets (MIT, Copyright (c) 2026 jhaizhou).
import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import dmhost from 'libdmhost.so';

const DOMAIN: number = 0xD2E1;
const TAG: string = 'nx2a-ohos';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'Failed to set colorMode: %{public}s', JSON.stringify(err));
    }
    hilog.info(DOMAIN, TAG, 'nx2a-ohos boot: UIAbility onCreate');
  }

  onForeground(): void {
    try { dmhost.hapticsSuspend(false); } catch (e) { }
  }

  onBackground(): void {
    try { dmhost.hapticsSuspend(true); } catch (e) { }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Landscape full screen (d2r's EntryAbility): hide the status/navigation bars and keep the screen on. Without this
    // the XComponent is 2860x1174 and the bottom ~98 px is the system navigation bar, not game letterboxing.
    windowStage.getMainWindow().then((win: window.Window) => {
      win.setWindowLayoutFullScreen(true);
      win.setWindowSystemBarEnable([]);
      win.setWindowKeepScreenOn(true);
    }).catch((e: Error) => hilog.warn(DOMAIN, TAG, 'window setup: %{public}s', e.message));
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, 'nx2a-ohos boot: pages/Index loaded');
    });
  }
}
