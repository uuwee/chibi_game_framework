#ifndef CGF_ENGINE_LOADER_H
#define CGF_ENGINE_LOADER_H

#include "renderer.h"

#define CGF_LOADER_API_NAME "cgf.loader"

/* 読み込み中または読み込んだアセット（モデルや画像）を指すハンドル。
   CGF_ID と同じく、値に意味はなく、ゼロ初期化したハンドルは無効。 */
typedef CGF_ID CGF_Asset;

typedef enum CGF_AssetStatus {
  CGF_ASSET_STATUS_LOADING, /* 読み込み中 */
  CGF_ASSET_STATUS_READY,   /* 読み込みが終わり、使える */
  CGF_ASSET_STATUS_FAILED,  /* 読み込みに失敗した。理由はログに出る */
  CGF_ASSET_STATUS_INVALID, /* 無効なハンドル */
} CGF_AssetStatus;

/* モデルの読み込み方法。 */
typedef struct CGF_ModelLoadDesc {
  /* FBX ファイルのパス。テクスチャは、ファイルに記録された相対パスを
     このファイルのあるフォルダから探す。 */
  const char *path;
  /* 光源の明るさに掛ける倍率。0 のときは 1 として扱う。 */
  float light_scale;
  /* 自己発光の明るさに掛ける倍率。0 のときは 1 として扱う。 */
  float emissive_scale;
  /* 点光源が届く距離を決める明るさ。点光源は、color * intensity の最大成分を
     距離の 2 乗で割った値がこれを下回るところまで届く。0 のときは 0.1。 */
  float light_cutoff;
} CGF_ModelLoadDesc;

/* 読み込んだモデルの情報。 */
typedef struct CGF_ModelInfo {
  /* すべてのインスタンスを囲む箱（ワールド座標）。 */
  float bounds_min[3];
  float bounds_max[3];
  /* ファイルにカメラがあれば true になり、最初のカメラの位置・向きと
     縦の画角（ラジアン）が入る。 */
  bool has_camera;
  float camera_position[3];
  float camera_forward[3];
  float camera_up[3];
  float camera_fov_y;
  uint32_t mesh_count;
  uint32_t material_count;
  uint32_t texture_count;
  uint32_t instance_count;
  uint32_t light_count;
} CGF_ModelInfo;

/* ローダー API。レジストリから CGF_LOADER_API_NAME で取得する。
   レンダラー API（CGF_ENGINE_RENDERER_API_NAME）が使える状態で使う。

   - 読み込みは別スレッドで進み、その間もフレームを描画し続けてよい。
     読み込みが終わったかは status で調べる。
   - 読み込んだデータは、レンダラーのメッシュ・テクスチャ・マテリアル・
     インスタンス・光源として作成される。これらはアセットが持ち、
     destroy で破棄される。呼び出し側で個別に破棄してはならない。
   - 座標系は Y 軸上向きの右手系で、単位はメートルに変換される。
   - すべての関数は任意のスレッドから呼んでよい。 */
typedef struct CGF_LoaderAPI {
  /* desc のモデルの読み込みを始め、out_asset にハンドルを書き込む。
     引数が正しくないか、同時に読み込めるアセットの数を超えると false。 */
  bool (*model_load)(const CGF_ModelLoadDesc *desc, CGF_Asset *out_asset);

  /* path の画像をテクスチャとして読み込み始め、out_asset にハンドルを
     書き込む。対応する形式は Radiance HDR（.hdr、RGBA16_FLOAT のミップ付き
     テクスチャになる）と DDS（.dds）。DDS の色は sRGB として読む。 */
  bool (*image_load)(const char *path, CGF_Asset *out_asset);

  /* アセットの状態を返す。out_progress が NULL でなければ、進み具合を
     0..1 で書き込む。 */
  CGF_AssetStatus (*status)(CGF_Asset asset, float *out_progress);

  /* READY のモデルの情報を out_info に書き込む。それ以外では false。 */
  bool (*model_info)(CGF_Asset asset, CGF_ModelInfo *out_info);

  /* READY のモデルのすべてのインスタンスと光源を scene に追加する。
     READY でなければ CGF_RENDER_ERROR_INVALID_STATE、asset が無効なら
     CGF_RENDER_ERROR_INVALID_HANDLE を返す。それ以外はレンダラーの
     scene_add / scene_add_lights の結果を返す。 */
  CGF_RenderResult (*model_add_to_scene)(CGF_Asset asset, CGF_Scene scene);

  /* READY の画像のテクスチャを返す。それ以外ではゼロのハンドル。 */
  CGF_Texture (*image_texture)(CGF_Asset asset);

  /* アセットを破棄する。読み込み中なら中断する（戻るまで待つことがある）。
     作成したレンダラーのリソースもすべて破棄する。無効なハンドルは無視する。 */
  void (*destroy)(CGF_Asset asset);
} CGF_LoaderAPI;

#endif
