# QuickJS External Generator Plugin (Plan B)

`v-simple-editor.exe` 本体の配布サイズを増やさずに、軽量 JavaScript スクリプトでモーションクリップを描画するための任意追加プラグインです。

## インストール先
ビルドした `vse-quickjs-gen.exe` と `generator.json` を次のいずれかへ配置してください:
- `<v-simple-editor.exe のフォルダ>/plugins/generators/quickjs/`
- `%LOCALAPPDATA%/v-simple-editor/plugins/generators/quickjs/`
- または環境変数 `VEDITOR_GENERATOR_PLUGIN_DIR` で指定したフォルダ配下

## スクリプト仕様 (`renderFrame`)
サンプルは `../samples/kinetic-title.js` を参照してください。
未導入環境でプロジェクトを開いた場合でも、最後に描いたキャッシュ画像または「ジェネレータ未導入」カードが表示され、安全に開けます。
