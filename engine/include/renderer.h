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
typedef CGF_ID CGF_Texture;
typedef CGF_ID CGF_Light;

/* 行列はすべて列優先 (column-major) の 4x4 で、GLSL / Slang の mat4
   と同じ並び。 クリップ空間は Vulkan の規約（Y 下向き、深度 0..1）に従う。 */
typedef struct CGF_Camera {
  float view[16];       /* ワールド → ビュー */
  float projection[16]; /* ビュー → クリップ */
  /* 露出の補正（段数）。0 で等倍、+1 で 2 倍、-1 で半分の明るさになる。 */
  float exposure;
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

/* 頂点形式。
   UV は Vulkan の規約に従い、(0, 0) がテクスチャの先頭行の左端、
   v が増える向きが画像の下向きになる。 */
typedef struct CGF_Vertex {
  float position[3];
  float normal[3];
  float uv[2];
  /* 接線。xyz は u が増える向きの単位ベクトル。
     w は +1 か -1 で、cross(normal, tangent.xyz) * w が v の増える向きになる。
     法線テクスチャを使うマテリアルで必要になる。 */
  float tangent[4];
} CGF_Vertex;

/* メッシュの元データ。三角形リストで、index_count は 3 の倍数。
   表面は、反時計回りに頂点が並んで見える側。
   mesh_create の中でコピーされるので、呼び出し後に解放してよい。 */
typedef struct CGF_MeshData {
  const CGF_Vertex *vertex_list;
  uint32_t vertex_count;
  const uint32_t *index_list;
  uint32_t index_count;
} CGF_MeshData;

/* テクスチャのピクセル形式。
   _SRGB は sRGB で符号化された色、_UNORM はそのままの値として読まれる。
   BC 形式は 4x4 ピクセルのブロック単位で圧縮されたデータで、
   ブロック 1 つのバイト数は BC1 / BC4 が 8、BC3 / BC5 / BC7 が 16。 */
typedef enum CGF_TextureFormat {
  CGF_TEXTURE_FORMAT_RGBA8_UNORM,
  CGF_TEXTURE_FORMAT_RGBA8_SRGB,
  CGF_TEXTURE_FORMAT_RGBA16_FLOAT,
  CGF_TEXTURE_FORMAT_RGBA32_FLOAT,
  CGF_TEXTURE_FORMAT_BC1_UNORM, /* RGBA（A は 0 か 1） */
  CGF_TEXTURE_FORMAT_BC1_SRGB,
  CGF_TEXTURE_FORMAT_BC3_UNORM, /* RGBA */
  CGF_TEXTURE_FORMAT_BC3_SRGB,
  CGF_TEXTURE_FORMAT_BC4_UNORM, /* R */
  CGF_TEXTURE_FORMAT_BC5_UNORM, /* RG */
  CGF_TEXTURE_FORMAT_BC7_UNORM, /* RGBA */
  CGF_TEXTURE_FORMAT_BC7_SRGB,
} CGF_TextureFormat;

/* テクスチャの元データ。
   data には、ミップレベル 0（width x height）から順に mip_count 個のミップを
   隙間なく並べる。各ミップの幅と高さは 1 つ前の半分（切り捨て、最小 1）。
   各ミップは先頭行から順に並べ、BC 形式ではブロックの行を上から順に並べる。
   size は data のバイト数で、上の並びの合計と一致しなければならない。
   texture_create の中でコピーされるので、呼び出し後に解放してよい。 */
typedef struct CGF_TextureData {
  CGF_TextureFormat format;
  uint32_t width;
  uint32_t height;
  uint32_t mip_count; /* 1 以上。ミップを最後（1x1）まで含める必要はない */
  const void *data;
  size_t size;
} CGF_TextureData;

/* マテリアルのブレンド方法。 */
typedef enum CGF_MaterialBlend {
  /* 不透明。アルファは使わない。 */
  CGF_MATERIAL_BLEND_OPAQUE,
  /* 切り抜き。アルファが alpha_cutoff 未満の部分は描画されない。 */
  CGF_MATERIAL_BLEND_MASK,
  /* 半透明。アルファを不透明度として、奥にあるものに重ねて描く。 */
  CGF_MATERIAL_BLEND_ALPHA,
} CGF_MaterialBlend;

/* マテリアルの元データ。金属度・粗さモデルの PBR マテリアル。
   各値は、対応するテクスチャがあればその値と掛け合わされる。
   テクスチャのハンドルは、使わない場合はゼロ初期化したままにする。 */
typedef struct CGF_MaterialData {
  /* 描画状態 */
  CGF_MaterialBlend blend;
  bool double_sided;  /* true なら裏面も描画する */
  float alpha_cutoff; /* MASK のときのアルファのしきい値 */

  /* パラメータ */
  float base_color[4]; /* RGBA、リニア。A はアルファ */
  float emissive[3];   /* 自己発光の放射輝度（RGB、リニア）。周囲も照らす */
  float metallic;      /* 金属度 0..1 */
  float roughness;     /* 粗さ 0..1 */
  /* 遮蔽テクスチャ（occlusion_roughness_metallic_texture の R）を効かせる
     強さ 0..1。0 なら R を使わず、1 なら R をそのまま使う */
  float occlusion_strength;

  /* テクスチャ */
  /* RGBA。base_color に掛ける。色は _SRGB 形式で渡すのが普通 */
  CGF_Texture base_color_texture;
  /* 接線空間の法線。R が接線（u の増える向き）、G が v の増える向き、
     B が法線の成分で、それぞれ 0..1 を -1..1 に対応させる。
     B は使わずに R と G から求めるので、BC5 などの 2 チャンネルの形式でよい */
  CGF_Texture normal_texture;
  /* R: 遮蔽（環境光が届く割合）、G: 粗さ、B: 金属度。
     R は occlusion_strength に従って使い、G は roughness に、B は metallic
     に掛ける */
  CGF_Texture occlusion_roughness_metallic_texture;
  /* RGB。emissive に掛ける */
  CGF_Texture emissive_texture;
} CGF_MaterialData;

/* 光源の種類。 */
typedef enum CGF_LightType {
  /* 点光源。position から全方向に光り、距離の 2 乗に反比例して暗くなる。 */
  CGF_LIGHT_TYPE_POINT,
  /* 平行光源。direction の向きに、どこでも同じ明るさで照らす（太陽など）。 */
  CGF_LIGHT_TYPE_DIRECTIONAL,
} CGF_LightType;

/* 光源の元データ。 */
typedef struct CGF_LightData {
  CGF_LightType type;
  float position[3];  /* POINT: ワールド座標 */
  float radius;       /* POINT: 光が届く最大距離。これより遠くは照らさない */
  float direction[3]; /* DIRECTIONAL: 光が進む向き。長さは任意（0 以外） */
  float color[3];     /* RGB、リニア */
  float intensity;    /* 明るさ。color に掛けられる */
  /* DIRECTIONAL: true なら影を落とす。1 つのシーンで影を落とす平行光源は
     1 つだけで、該当するものが複数あるときは color * intensity の最も明るい
     ものが選ばれる。POINT では無視される（点光源は影を落とさない） */
  bool cast_shadow;
} CGF_LightData;

/* 既存の光源の更新。data のすべてのフィールドで置き換えられる。 */
typedef struct CGF_LightUpdate {
  CGF_Light light;
  CGF_LightData data;
} CGF_LightUpdate;

/* シーンを囲む環境（空）。 */
typedef struct CGF_EnvironmentData {
  /* 全天の放射輝度を正距円筒図法（横 360 度、縦 180 度、上端が真上）で
     表したテクスチャ。リニアな値として読まれるので、_FLOAT の形式を使う。
     ミップがあると、粗い面の映り込みがなめらかになる */
  CGF_Texture texture;
  float intensity; /* テクスチャの値に掛ける倍率 */
  float rotation;  /* 真上を軸とした回転（ラジアン） */
} CGF_EnvironmentData;

/* レンダラーの状態。get_stats で取得する。 */
typedef struct CGF_RenderStats {
  /* GPU への転送を待っているデータの合計バイト数。 */
  uint64_t pending_upload_bytes;
  /* 転送が終わっておらず、まだ描画に使われないメッシュとテクスチャの数。 */
  uint32_t pending_mesh_count;
  uint32_t pending_texture_count;
} CGF_RenderStats;

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
   - 同時に呼ばれた関数の結果は、それらを何らかの順に 1 つずつ呼んだ場合と
     同じになる。1 回の呼び出しによる変更は、フレームに対してまとめて反映される
     （リストの一部だけが描画に反映されることはない）。
   - あるスレッドで作成したハンドルを別のスレッドで使うときは、
     呼び出し側でハンドルの受け渡しを同期させる。

   変更の反映:
   - frame_begin を呼ぶ前に戻った関数の変更は、そのフレームに反映される。
     frame_begin から戻った後に呼ばれた関数の変更は、次のフレームから
     反映される。frame_begin と並行して呼ばれた関数の変更は、そのどちらかに
     反映される。
   - ただし、メッシュとテクスチャのデータは、作成後のフレームで少しずつ GPU
     に転送される。転送が終わるまで、そのメッシュを使うインスタンスは描画
     されず、そのテクスチャはマテリアルや環境から使われない（テクスチャが
     ないものとして描画される）。転送の状況は get_stats で分かる。
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
     シーン: 描画するインスタンスと光源の集まり。階層構造は持たない。
     1 つのインスタンスや光源を複数のシーンに入れてよい。 */

  /* 空のシーンを作成し、out_scene に書き込む。
     環境は設定されていない状態で、frame_begin の clear_color が使われる。 */
  CGF_RenderResult (*scene_create)(CGF_Scene *out_scene);

  /* シーンを破棄する。シーンに入っていたインスタンスや光源は破棄されない。 */
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

  /* light_list の count 個の光源をシーンに追加する。
     1 つでも無効なハンドルがあれば CGF_RENDER_ERROR_INVALID_HANDLE を返し、
     何も追加しない。
     同じ光源を同じシーンに重複して追加した場合の動作は未定義。 */
  CGF_RenderResult (*scene_add_lights)(CGF_Scene scene,
                                       const CGF_Light *light_list,
                                       uint32_t count);

  /* light_list の count 個の光源をシーンから外す。
     光源自体は破棄されない。シーンに入っていない光源や
     破棄済みのハンドルは無視される。 */
  void (*scene_remove_lights)(CGF_Scene scene, const CGF_Light *light_list,
                              uint32_t count);

  /* シーンの環境を設定する。環境は空として背景に描かれ、シーンを照らし、
     表面に映り込む。
     environment が NULL なら環境の設定を外し、frame_begin の clear_color
     を使う状態に戻す。
     environment->texture は有効なハンドルでなければならない。
     後でテクスチャが破棄されると、環境を設定していない状態として描画される。 */
  CGF_RenderResult (*scene_set_environment)(
      CGF_Scene scene, const CGF_EnvironmentData *environment);

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

  /* update_list の count 個の更新を適用する。各要素は mask で指定した
     フィールドだけを書き換える。mask に MESH / MATERIAL を含む場合、
     新しい mesh / material は有効なハンドルでなければならない。
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
     テクスチャ */

  /* data_list の count 個のデータからテクスチャを作成し、
     out_texture_list に同じ順でハンドルを書き込む。
     この呼び出しは時間がかかることがある。 */
  CGF_RenderResult (*texture_create)(const CGF_TextureData *data_list,
                                     uint32_t count,
                                     CGF_Texture *out_texture_list);

  /* テクスチャを破棄する。このテクスチャを使っているマテリアルや環境は、
     テクスチャがないものとして描画される。 */
  void (*texture_destroy)(CGF_Texture texture);

  /* ------------------------------------------------------------------------
     マテリアル */

  /* data_list の count 個のデータからマテリアルを作成し、
     out_material_list に同じ順でハンドルを書き込む。
     各データのテクスチャは、使う場合はこの時点で有効なハンドルでなければ
     ならない。 */
  CGF_RenderResult (*material_create)(const CGF_MaterialData *data_list,
                                      uint32_t count,
                                      CGF_Material *out_material_list);

  /* マテリアルを破棄する。このマテリアルを使っているインスタンスは描画されなくなる。
     インスタンスの material は自動では変わらないので、再び描画するには
     instance_update で別のマテリアルに差し替える。 */
  void (*material_destroy)(CGF_Material material);

  /* ------------------------------------------------------------------------
     光源: 作成した時点ではどのシーンにも入っていないので、
     照らすには scene_add_lights が必要。 */

  /* data_list の count 個のデータから光源を作成し、
     out_light_list に同じ順でハンドルを書き込む。 */
  CGF_RenderResult (*light_create)(const CGF_LightData *data_list,
                                   uint32_t count, CGF_Light *out_light_list);

  /* light_list の count 個の光源を破棄する。
     シーンに入ったままでも破棄してよく、破棄後はどのシーンも照らさない。 */
  void (*light_destroy)(const CGF_Light *light_list, uint32_t count);

  /* update_list の count 個の更新を適用する。
     同じ光源への更新が複数あるときは、リストの後ろにあるものが優先される。 */
  CGF_RenderResult (*light_update)(const CGF_LightUpdate *update_list,
                                   uint32_t count);

  /* ------------------------------------------------------------------------
     状態 */

  /* レンダラーの状態を out_stats に書き込む。 */
  void (*get_stats)(CGF_RenderStats *out_stats);

  /* ------------------------------------------------------------------------
     フレーム */

  /* フレームを開始し、ウィンドウ全体を clear_color（RGBA、リニア）で塗りつぶす。
     環境を設定していないシーンでは、clear_color の RGB がこのフレームで
     シーンを囲む環境（空）の明るさとして使われ、シーンを間接的に照らす。
     GPU の処理が追いつくまで待つことがある。
     ウィンドウに描画できない場合は CGF_RENDER_NOT_VISIBLE を返す。
     前のフレームの frame_end を呼ぶ前に呼ぶと CGF_RENDER_ERROR_INVALID_STATE を返す。 */
  CGF_RenderResult (*frame_begin)(const float clear_color[4]);

  /* scene を camera の視点で、ウィンドウ全体に描画する。
     1 フレームの中で複数回呼んでよく、後の render は前の render の上に重ねて
     描かれる。深度は render ごとにクリアされるので、異なる render の
     インスタンス同士は前後関係で隠し合わない。
     環境を設定したシーンは、何も描かれない部分に空を描くので、それより前の
     render の内容を覆い隠す。環境を設定していないシーンでは、何も描かれない
     部分は前の内容が残る。
     scene はこのフレームの状態で判定され、frame_begin より前に破棄されていれば
     CGF_RENDER_ERROR_INVALID_HANDLE を返す。
     frame_begin と frame_end の間以外で呼ぶと CGF_RENDER_ERROR_INVALID_STATE
     を返す。 */
  CGF_RenderResult (*render)(CGF_Scene scene, const CGF_Camera *camera);

  /* フレームを終了し、描画した内容をウィンドウに表示する。
     frame_begin の後以外で呼ぶと CGF_RENDER_ERROR_INVALID_STATE を返す。 */
  CGF_RenderResult (*frame_end)(void);
} CGF_EngineRendererAPI;

#endif
