#ifndef CGF_ENGINE_RENDERER_H
#define CGF_ENGINE_RENDERER_H

#include "../../runtime/include/runtime.h"

#include <stdint.h>

#define CGF_ENGINE_RENDERER_API_NAME "cgf.engine.renderer"

/* レンダラーのリソースを指すハンドル。
   フィールドの値に意味はなく、呼び出し側は値を解釈・加工してはならない。
   ゼロ初期化したハンドルは常に無効なハンドルになる。
   ハンドルは値としてコピー・保存してよく、プラグインの永続ステートに置いて
   ホットリロード後も使い続けられる。 */
typedef struct CGF_ID {
  uint32_t id, gen;
} CGF_ID;

typedef CGF_ID CGF_Scene;
typedef CGF_ID CGF_Instance;
typedef CGF_ID CGF_Mesh;
typedef CGF_ID CGF_Material;

/* 行列はすべて列優先 (column-major) の 4x4 で、GLSL / Slang の mat4
   と同じ並び。 クリップ空間は Vulkan の規約（Y 下向き、深度 0..1）に従う。 */
typedef struct CGF_Camera {
  float view[16];       /* ワールド → ビュー */
  float projection[16]; /* ビュー → クリップ */
} CGF_Camera;

/* インスタンス 1 つ分の描画情報。
   mesh / material が破棄されると、そのインスタンスは描画されなくなる。 */
typedef struct CGF_InstanceData {
  float transform[16]; /* オブジェクト → ワールド */
  CGF_Mesh mesh;
  CGF_Material material;
} CGF_InstanceData;

/* CGF_InstanceUpdate.mask に指定するビット。OR で組み合わせて使う。 */
typedef enum CGF_RenderUpdate {
  CGF_RENDER_UPDATE_TRANSFORM = 1u << 0,
  CGF_RENDER_UPDATE_MESH = 1u << 1,
  CGF_RENDER_UPDATE_MATERIAL = 1u << 2,
} CGF_RenderUpdate;

/* CGF_RenderUpdate のビットの組み合わせ。 */
typedef uint32_t CGF_RenderUpdateFlags;

/* 既存インスタンスの部分更新。
   mask で指定したフィールドだけが data から反映され、それ以外は無視される。 */
typedef struct CGF_InstanceUpdate {
  CGF_Instance instance;
  CGF_RenderUpdateFlags mask;
  CGF_InstanceData data;
} CGF_InstanceUpdate;

/* 頂点形式。 */
typedef struct CGF_Vertex {
  float position[3];
  float normal[3];
  float uv[2];
} CGF_Vertex;

/* メッシュの元データ。三角形リストで、index_count は 3 の倍数。
   mesh_create の中でコピーされるので、呼び出し後に解放してよい。 */
typedef struct CGF_MeshData {
  const CGF_Vertex *vertex_list;
  uint32_t vertex_count;
  const uint32_t *index_list;
  uint32_t index_count;
} CGF_MeshData;

/* マテリアルのブレンド方法。 */
typedef enum CGF_MaterialBlend {
  CGF_MATERIAL_BLEND_OPAQUE,
  CGF_MATERIAL_BLEND_ALPHA,
} CGF_MaterialBlend;

/* マテリアルの元データ。
   描画状態を決めるフィールドと、色などのパラメータからなる。 */
typedef struct CGF_MaterialData {
  /* 描画状態 */
  CGF_MaterialBlend blend;
  bool double_sided; /* true なら裏面も描画する */

  /* パラメータ */
  float base_color[4]; /* RGBA、リニア */
} CGF_MaterialData;

typedef enum CGF_RenderResult {
  CGF_RENDER_OK = 0,
  /* ウィンドウが開かれていない、または最小化されていて描画できない。
     エラーではなく、frame_begin だけが返す。 */
  CGF_RENDER_NOT_VISIBLE,
  CGF_RENDER_ERROR_INVALID_ARGUMENT, /* NULL ポインタ、範囲外の値など */
  CGF_RENDER_ERROR_INVALID_STATE,    /* フレームの呼び出し順が正しくない */
  CGF_RENDER_ERROR_INVALID_HANDLE,   /* 無効または破棄済みのハンドル */
  CGF_RENDER_ERROR_CAPACITY,         /* 作成できる数の上限に達した */
  CGF_RENDER_ERROR_OUT_OF_MEMORY,    /* メモリの確保に失敗した */
  CGF_RENDER_ERROR_DEVICE,           /* GPU のエラー（デバイスロストなど） */
} CGF_RenderResult;

/* レンダラー API。レジストリから CGF_ENGINE_RENDERER_API_NAME で取得する。

   描画先:
   - 描画先は Engine が開いたウィンドウだけで、レンダラーが自動的に使う。
     ウィンドウのサイズ変更には自動で追従する。

   フレーム:
   - 1 フレームは frame_begin → render（0 回以上）→ frame_end の順に呼ぶ。
     frame_begin が CGF_RENDER_OK 以外を返したフレームでは、
     render と frame_end を呼ばない。

   スレッド:
   - frame_begin / render / frame_end（フレーム関数）は、同時に 2 つ以上の
     スレッドから呼んではならない。
   - フレーム関数以外の関数は、任意のスレッドから同時に呼んでよく、
     フレームの途中でも呼んでよい。
   - 同時に呼ばれた関数の結果は、それらを何らかの順に 1
   つずつ呼んだ場合と同じになる。 1
   回の呼び出しによる変更は、フレームに対してまとめて反映される
     （リストの一部だけが描画に反映されることはない）。
   - あるスレッドで作成したハンドルを別のスレッドで使うときは、
     呼び出し側でハンドルの受け渡しを同期させる。

   変更の反映:
   - frame_begin を呼ぶ前に戻った関数の変更は、そのフレームに反映される。
     frame_begin
   から戻った後に呼ばれた関数の変更は、次のフレームから反映される。 frame_begin
   と並行して呼ばれた関数の変更は、そのどちらかに反映される。
   - 1 フレームの中のすべての render は、同じ時点の状態を描画する。
   - 作成したハンドルは、関数から戻った直後から他の関数に渡してよい
     （instance_create → scene_add のように続けて使える）。

   エラー:
   - CGF_RenderResult を返す関数は、失敗したときに何も変更しない
     （リストを受け取る関数は全件成功か全件失敗のどちらか）。
   - void を返す破棄・除去系の関数は、無効なハンドルを黙って無視する。
   - リストと count を受け取る関数は、count == 0 なら何もせず CGF_RENDER_OK
   を返す。

   破棄:
   - リソースはフレームの途中でも、描画の完了を待たずにいつでも破棄してよい。
     破棄したハンドルは関数の引数としては直後から無効になるが、描画への反映は
     「変更の反映」の規則に従う。 */
typedef struct CGF_EngineRendererAPI {
  /* ------------------------------------------------------------------------
     シーン: 描画するインスタンスの集まり。階層構造は持たない。
     1 つのインスタンスを複数のシーンに入れてよい。 */

  /* 空のシーンを作成し、out_scene に書き込む。 */
  CGF_RenderResult (*scene_create)(CGF_Scene *out_scene);

  /* シーンを破棄する。シーンに入っていたインスタンスは破棄されない。 */
  void (*scene_destroy)(CGF_Scene scene);

  /* instance_list の count 個のインスタンスをシーンに追加する。
     1 つでも無効なハンドルがあれば CGF_RENDER_ERROR_INVALID_HANDLE を返し、
     何も追加しない。
     同じインスタンスを同じシーンに重複して追加した場合の動作は未定義。 */
  CGF_RenderResult (*scene_add)(CGF_Scene scene,
                                const CGF_Instance *instance_list,
                                uint32_t count);

  /* instance_list の count 個のインスタンスをシーンから外す。
     インスタンス自体は破棄されない。シーンに入っていないインスタンスや
     破棄済みのハンドルは無視される。 */
  void (*scene_remove)(CGF_Scene scene, const CGF_Instance *instance_list,
                       uint32_t count);

  /* ------------------------------------------------------------------------
     インスタンス: メッシュ・マテリアル・transform の組。
     作成した時点ではどのシーンにも入っていないので、描画するには scene_add
     が必要。 */

  /* data_list の count 個のデータからインスタンスを作成し、
     out_instance_list に同じ順でハンドルを書き込む。
     各データの mesh / material は、この時点で有効なハンドルでなければならない。
   */
  CGF_RenderResult (*instance_create)(const CGF_InstanceData *data_list,
                                      uint32_t count,
                                      CGF_Instance *out_instance_list);

  /* instance_list の count 個のインスタンスを破棄する。
     シーンに入ったままでも破棄してよく、破棄後はどのシーンでも描画されない。
     シーンから外す必要はない。 */
  void (*instance_destroy)(const CGF_Instance *instance_list, uint32_t count);

  /* update_list の count 個の更新を適用する。各要素は mask
     で指定したフィールドだけを 書き換える。mask に MESH / MATERIAL
     を含む場合、新しい mesh / material は 有効なハンドルでなければならない。
     同じインスタンスへの更新が複数あるときは、リストの後ろにあるものが優先される。
   */
  CGF_RenderResult (*instance_update)(const CGF_InstanceUpdate *update_list,
                                      uint32_t count);

  /* ------------------------------------------------------------------------
     メッシュ */

  /* data_list の count 個のデータからメッシュを作成し、
     out_mesh_list に同じ順でハンドルを書き込む。 */
  CGF_RenderResult (*mesh_create)(const CGF_MeshData *data_list, uint32_t count,
                                  CGF_Mesh *out_mesh_list);

  /* メッシュを破棄する。このメッシュを使っているインスタンスは描画されなくなる。
     インスタンスの mesh は自動では変わらないので、再び描画するには
     instance_update で別のメッシュに差し替える。 */
  void (*mesh_destroy)(CGF_Mesh mesh);

  /* ------------------------------------------------------------------------
     マテリアル */

  /* data_list の count 個のデータからマテリアルを作成し、
     out_material_list に同じ順でハンドルを書き込む。
     この呼び出しは時間がかかることがある。 */
  CGF_RenderResult (*material_create)(const CGF_MaterialData *data_list,
                                      uint32_t count,
                                      CGF_Material *out_material_list);

  /* マテリアルを破棄する。このマテリアルを使っているインスタンスは描画されなくなる。
     インスタンスの material は自動では変わらないので、再び描画するには
     instance_update で別のマテリアルに差し替える。 */
  void (*material_destroy)(CGF_Material material);

  /* ------------------------------------------------------------------------
     フレーム */

  /* フレームを開始し、ウィンドウ全体を
     clear_color（RGBA、リニア）で塗りつぶす。 GPU
     の処理が追いつくまで待つことがある。 ウィンドウに描画できない場合は
     CGF_RENDER_NOT_VISIBLE を返す。 前のフレームの frame_end を呼ぶ前に呼ぶと
     CGF_RENDER_ERROR_INVALID_STATE を返す。 */
  CGF_RenderResult (*frame_begin)(const float clear_color[4]);

  /* scene を camera の視点で、ウィンドウ全体に描画する。
     1 フレームの中で複数回呼んでよく、後の render は前の render
     の上に重ねて描かれる。 深度は render ごとにクリアされるので、異なる render
     のインスタンス同士は 前後関係で隠し合わない。 scene
     はこのフレームの状態で判定され、frame_begin より前に破棄されていれば
     CGF_RENDER_ERROR_INVALID_HANDLE を返す。
     frame_begin と frame_end の間以外で呼ぶと CGF_RENDER_ERROR_INVALID_STATE
     を返す。 */
  CGF_RenderResult (*render)(CGF_Scene scene, const CGF_Camera *camera);

  /* フレームを終了し、描画した内容をウィンドウに表示する。
     frame_begin の後以外で呼ぶと CGF_RENDER_ERROR_INVALID_STATE を返す。 */
  CGF_RenderResult (*frame_end)(void);
} CGF_EngineRendererAPI;

#endif
