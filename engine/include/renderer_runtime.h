#ifndef CGF_ENGINE_RENDERER_RUNTIME_H
#define CGF_ENGINE_RENDERER_RUNTIME_H

#include "../../runtime/include/runtime.h"

/* ランタイムだけが使うレンダラーの API。ゲームなどの利用側は使わない。 */

#define CGF_ENGINE_RENDERER_RUNTIME_API_NAME "cgf.engine.renderer.runtime"

typedef struct CGF_EngineRendererRuntimeAPI {
  /* レンダラーが持つすべての GPU リソースを解放する。
     ランタイムの終了時、プラグインを閉じる前に 1 度だけ呼ぶ。
     呼んだ後は、レンダラー API のどの関数も呼んではならない。 */
  void (*shutdown)(void);
} CGF_EngineRendererRuntimeAPI;

/* レンダラープラグインの入口。CGF_ENGINE_API_NAME の Engine API と、
   そのウィンドウが開かれている必要がある。
   2 回目以降のロードでは、作成済みのリソースとハンドルを引き継ぐ。 */
CGF_PLUGIN_EXPORT bool cgf_renderer_load(const CGF_APIRegistry *registry);

#endif
