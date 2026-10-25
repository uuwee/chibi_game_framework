#ifndef CGF_ENGINE_LOADER_RUNTIME_H
#define CGF_ENGINE_LOADER_RUNTIME_H

#include "../../runtime/include/runtime.h"

/* ランタイムだけが使うローダーの API。ゲームなどの利用側は使わない。 */

#define CGF_LOADER_RUNTIME_API_NAME "cgf.loader.runtime"

typedef struct CGF_LoaderRuntimeAPI {
  /* 読み込み中のアセットをすべて中断し、ローダーのメモリを解放する。
     ランタイムの終了時、レンダラーより先に 1 度だけ呼ぶ。
     作成済みのレンダラーのリソースは破棄しない（レンダラーの終了で解放される）。
     呼んだ後は、ローダー API のどの関数も呼んではならない。 */
  void (*shutdown)(void);

  /* 読み込みスレッドがレンダラーを呼ぶのを止める。レンダラーの関数の
     途中にいるスレッドがあれば、出てくるまで待つ。
     レンダラーをリロードする前に呼び、終わったら resume を呼ぶ。 */
  void (*pause)(void);
  void (*resume)(void);
} CGF_LoaderRuntimeAPI;

/* ローダープラグインの入口。 */
CGF_PLUGIN_EXPORT bool cgf_loader_load(const CGF_APIRegistry *registry);

#endif
