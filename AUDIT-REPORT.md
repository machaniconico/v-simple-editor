# AUDIT-REPORT: v-simple-editor `feat/nle-gap-wave18`(f483e21)

## 全体所見
- 重点 3 領域に Critical(データ全損・常時クラッシュ)に当たるものは見つかりませんでした。重大度が高いのは「AI チャットから任意のパスへファイルを書ける」「式パーサが再帰の深さを制限していない」「位置キーで紐付く sidecar がシーケンスをまたぐ」の 3 点です。
- wave18 で追加された部分(worldX/Y、点群 JSON、3D テキスト/ブラシのクリップ移行、split 時のシフト、mergeDuckingEnvelope)は、保存→読込の往復でも書き出しでも、大きな破綻は見つかりませんでした。
- 書き出しとプレビューの不一致は、主に「カット点ちょうどの区間判定(閉区間と半開区間)」と「24/48/12fps のフレーム時刻の切り捨て」から起きています。
- 保存まわりでは、未保存変更を確認する仕組みがないこと、アトミックでない自動保存、版番号を上げていないことが、利用者の作業消失に直結します。
- この環境ではビルドが通らなかったため(末尾参照)、どの指摘も静的に追跡した結果です。行番号はすべて f483e21 時点のものです。

---

## 指摘(重大度順)

### [High] AI チャット(読み取り専用サンドボックス)から、MCP 経由で任意のパスにファイルを書ける
- 場所:
  - src/AiChatDock.cpp:289, :303(Codex に `-s read-only` を付けつつ、veditor MCP には `default_tools_approval_mode='approve'`)
  - src/AiChatDock.cpp:490(Claude 用: `--allowedTools mcp__veditor`)
  - src/mcp/McpEditorTools.cpp:3171-3181(`save_project`)
  - src/mcp/McpEditorTools.cpp:2706-2717(`export_video` の outputPath)
- 問題:
  - `save_project` の `path` は空文字かどうかしか見ていません。拡張子の制限も、上書きの確認もないまま `saveProjectToPath` に渡ります。
  - `export_video` も、親ディレクトリがあるかしか確認していません。
  - エージェント自身は読み取り専用で動いていますが、書き込みはサンドボックスの外にあるエディタプロセスが代わりに行います。しかも承認なしで実行されます。
- 発生条件: プロンプトインジェクションを受けたエージェントが、たとえば次のように呼ぶ場合。
  - `save_project {"path":"/home/<user>/.bashrc"}`
  - `export_video {"outputPath":"<既存の素材ファイル>"}`
- 影響:
  - ユーザーのファイル(設定ファイル、素材、別のプロジェクト)を上書きして壊します。
  - トラック名など、エージェントが自由に決められる文字列が JSON に入るため、シェルの rc ファイル経由でコマンド実行につながる可能性があります(未検証)。
- 確度: 任意のパスへの書き込みは確認済みです(上記の箇所を読み直しました)。コード実行まで連鎖するかどうかは「可能性あり」で、実行していません。
- 修正方針:
  - `save_project` は自前の拡張子に限り、既存ファイルの上書きは同じ拡張子のときだけ許可する。
  - `export_video` はコンテナ形式の拡張子に限る。
  - AI チャット経由の呼び出しは、プロジェクトや出力フォルダの配下に書き込み先を限定する。

### [High] 式パーサに再帰の深さ上限がなく、MCP の `set_clip_expression` で入れた式でスタックが溢れてクラッシュする
- 場所:
  - src/Expression.cpp:280-292(`parseUnary` が自分自身を再帰呼び出し)
  - src/Expression.cpp:310-316(括弧の再帰)
  - src/mcp/McpEditorTools.cpp:4313-4321(`code` を検査しないまま `setExpression` に渡す)
- 問題:
  - パーサに深さのカウンタがありません。
  - MCP 側は `propPath` しか検証しておらず、`code` の長さや構文は見ていません。HTTP ボディの上限 4MB まで、そのまま通ります。
  - 評価は毎フレーム、プロパティごとに字句解析と構文解析をやり直します。
- 発生条件: `code` に `"-"` を数十万個並べたもの、または `"((((…1…))))"` のように深くネストした式を入れる。
- 影響:
  - 次の描画で、プロセスがスタックオーバーフローにより落ちます。
  - 式はプロジェクトと自動保存に残るため、開き直すたびに落ち続ける可能性があります。
- 確度: 深さ制限がないことは確認済みです。何段で落ちるかはスタックサイズ次第で、実際には動かしていません。
- 修正方針: パーサに深さの上限(例: 256)を設ける。MCP で `code` の長さに上限を設け、`Expression::validate` が失敗したら拒否する。

### [High] シーケンスを切り替えても、位置キー(`track:clip`)の roto / マット / タイムリマップの sidecar がそのまま残り、別シーケンスのクリップに付く
- 場所:
  - src/Timeline.cpp:16296-16348(`setActiveSequence` はトラックに `setClips` するだけで、`m_rotoEntries` などは入れ替えない)
  - src/Timeline.cpp:16187(コメント「Sequence navigation preserves the existing carrier keys」)
  - src/TimelineFrameRenderer.cpp:1854(roto を `renderClipId(t, clipIdx)` で引く)
  - src/MainWindow.cpp:8660, :9884(タイムリマップのエントリを全件、現在のトラックへ書き戻す)
- 問題: sidecar はシーケンスごとに分かれていません。シーケンス A の `0:0` に付いた roto やマットが、切り替え先 B の `0:0` のクリップに付きます。
- 発生条件: シーケンスが 2 つ以上あり、どれかに roto・マット・タイムリマップがある状態でシーケンスを切り替える(シーケンス一覧、パンくず、新規シーケンス、EDL/FCPXML の取り込み)。
- 影響:
  - 関係のないクリップがマスクされたり、時間がずれたりします。プレビューと書き出しの両方に出ます。
  - 保存すると B の位置として書き出されるため、その状態のまま残ります。
- 確度: 確認済みです(`setActiveSequence` の全体を読み直しました)。selftest は実行していません。
- 修正方針: sidecar をシーケンス id ごとに持たせ、切り替え時に入れ替える。または uid キーに統一する。

### [High] 非アクティブなシーケンスと `renderInPlaceOriginal` に入っているクリップのタイムリマップ曲線が、保存→読込で失われる
- 場所:
  - src/ProjectFile.cpp:1948-2100(`clipToJson` が `timeRemapCurve` を書かない)
  - src/ProjectFile.cpp:900(曲線を書くのはルートの `timeRemapClipEntries` だけ)
  - src/ProjectFile.cpp:1028-1053(`applyTimeRemapEntries` はアクティブなビデオトラックにしか適用しない)
  - src/Timeline.cpp:703(`sequenceToStoreJson` のときはルートのエントリが空)
- 問題: クリップの JSON に曲線が入らず、ルートのエントリもアクティブシーケンスの分しか作られません。
- 発生条件:
  - 非アクティブなシーケンス(ネストした子シーケンスを含む)に、タイムリマップやフリーズを持つクリップがある状態で保存→開く。
  - タイムリマップを持つクリップをレンダーインプレースし、保存→開いてから分解する。
- 影響: 開き直すと再生タイミングが変わります。プレビューも書き出しも、編集した時の見え方と一致しなくなります。
- 確度: 確認済みです(`clipToJson` に timeRemap のキーがないことを grep で確かめました)。
- 修正方針: `clipToJson` / `clipFromJson` で曲線をクリップ自身に持たせる。ルートのエントリは旧形式からの移行だけに使う。3D テキストのクリップ移行(US-1706)と同じ方式です。

### [High] 未保存の変更を確認せずに終了・新規作成・開くができ、終了時には自動保存の復元候補も止まる
- 場所:
  - src/MainWindow.cpp:20213-20221(`closeEvent` は確認なしで `markCleanShutdown()` を呼ぶ)
  - src/MainWindow.cpp:10183 `newProject`、:10345 `openProject`
- 問題: 保存・破棄・キャンセルを尋ねる処理がどこにもありません。`setWindowModified` は一部で設定されていますが、判定には使われていません。
- 発生条件: 編集してから保存せずに、ウィンドウを閉じる、新規作成する、または別のプロジェクトを開く。
- 影響:
  - 編集内容が警告なしに失われます。
  - 終了時に `.clean_shutdown` が書かれるため、次回起動しても自動保存からの復元が提示されません。
- 確度: 確認済みです(`closeEvent` を読み直しました)。この問題は wave18 以前からあります。
- 修正方針: undo のシリアル番号などで dirty を判定し、close / new / open の前に確認する。clean マーカーは保存したときか、破棄を選んだときだけ書く。

### [Medium] 24 / 48 / 12 fps で書き出すと、フレーム時刻の切り捨てによってカット点で 1 フレーム前のクリップが描かれる
- 場所:
  - src/RenderQueue.cpp:1794, :2294(`startUsec + static_cast<qint64>(f * usecPerFrame)`)
  - src/SequenceExport.cpp:75(同じ式)
  - 区間判定: src/TimelineFrameRenderer.cpp:1456(`targetSec < clipEnd` の半開区間)
- 問題:
  - 24fps では `usecPerFrame` が 41666.66… なので、f=99 のとき 4124999.999… になり、切り捨てで 4124999µs になります。
  - 4.125 秒ちょうどから始まるクリップ B ではなく、前のクリップ A が選ばれます。
  - プレビューと「現在フレームを書き出し」は `qRound64` を使うため、B が表示されます。
- 発生条件:
  - 24/48/12fps で、カットやキーフレームがちょうどフレーム境界にあり、そのフレーム時刻が整数 µs になる場合。
  - 1 時間あたり、24fps で 1584 フレーム、48fps で 3175、12fps で 789 が該当します(Python で数えました)。
  - 25/30/60fps では 0 件です。
- 影響: 書き出した動画で、B の先頭フレームの代わりに A の最後のフレームが出ます。キーフレームやテキストの開始も 1 フレーム遅れることがあります。
- 確度: 確認済みです(数値の計算と区間判定の両方を確かめました)。ただし、累積した `clipEnd` の浮動小数誤差で打ち消される場合があります。
- 修正方針: `startUsec + qRound64(f * 1e6 / fps)` に統一する。fps を有理数で持っているなら、整数演算にする。

### [Medium] 一時停止中の特殊合成プレビューで、カット点に出ていくクリップ A が手前に描かれる(wave18 の 7413de9 による退行)
- 場所:
  - src/MainWindow.cpp:8637-8639(V1 以外の採用判定に ±1/120 秒の許容幅がある)
  - src/MainWindow.cpp:8961-8966(マットがないフレームで `std::reverse`)
- 問題:
  - 同じトラックのカット点では A と B の両方が層に入ります。zOrder が同じなので、並びは [A, B] のままです。
  - reverse によって [B, A] になり、A が手前に描かれます。
  - 書き出し(半開区間)では B しか描きません。
- 発生条件: 一時停止中で、roto・タイムリマップ・3D テキスト・ライティングのいずれかを持つクリップが特殊合成経路を通り、マットがないこと。さらに、再生ヘッドがカット点からおよそ 1/120 秒以内にあること。
- 影響: カットの先頭フレームで、プレビューと書き出しの絵が違います。
- 確度: 確認済みです(許容幅と reverse の両方のコードを読み直しました)。ただし、ソートが安定かどうかは要素数と実装に依存します。
- 修正方針: V1 以外も半開区間で 1 トラック 1 クリップに絞る。または reverse の前に、同じトラックの層を後勝ちで間引く。

### [Medium] 単一クリップの GL プレビューで、カット点に前のクリップのモーションが当たる
- 場所: src/GLPreview.cpp:212-216(`timelineSec <= endSec + 1e-6` の閉区間で判定し、最初に一致したもので break)
- 問題: 表示されている画は VideoPlayer が半開区間で選んだ B です。一方、モーション(位置・スケール・不透明度・3D)は A のものが使われます。
- 発生条件: A と B の transform が違う状態で、カット点ちょうどに一時停止する。
- 影響: B の先頭フレームが、A の位置や大きさで表示されます。書き出しとは一致しません。
- 確度: 確認済みです(このコードは main にもあり、以前からの問題です)。
- 修正方針: 終端を `< endSec` にするか、同じトラックでは後のクリップを優先する。

### [Medium] 音声フェードを掛けるトランジションの種別と曲線が、プレビューと書き出しで違う
- 場所:
  - プレビュー: src/AudioMixer.cpp:971-993(`FadeIn || isOverlapTransition`、sqrt 曲線、lead と trail は `else if`)
  - 書き出し: src/Timeline.cpp:1027-1043(`FadeIn/FadeOut || CrossDissolve` のみ、`afade curve=qsin`、lead と trail は両方)
- 問題:
  - Wipe / Slide / Dip / Iris などの重なりトランジションは、映像側から音声クリップへミラーされます(Timeline.cpp:11799 付近)。プレビューでは音声がフェードしますが、書き出しではハードカットになります。
  - 曲線も sqrt と qsin で違います。
  - フェードがクリップ長の半分を超える短いクリップでは、書き出しだけが in と out の両方を掛けます。
- 発生条件: V1 のカットに Wipe などを付ける。または CrossDissolve や FadeIn を使う。
- 影響: プレビューで調整した音のつながりと、書き出した結果が違います。
- 確度:
  - 種別の差と else-if は確認済みです。
  - 曲線の差は可能性ありです。FFmpeg の qsin を sin(πt/2) とする理解は記憶によるもので、この環境ではソースを確認していません。Timeline.cpp:1023 のコメントは「一致する」と書いています。確認方法: FFmpeg の `libavfilter/af_afade.c` にある `fade_gain` の QSIN 分岐を参照してください。
- 修正方針: 対象の種別を `isOverlapTransition` に揃える。曲線と lead/trail の両立の扱いを、どちらか一方に統一する。

### [Medium] プロキシモードのプレビューで、ブラシの位置と大きさが書き出しと違う
- 場所:
  - src/MainWindow.cpp:8748, :8768, :8803(ブラシ経路のデコードにプロキシを使う)
  - src/BrushAnimation.cpp:238-279(絶対ピクセルで描き、縮尺しない)
- 問題: プロキシの解像度にネイティブ座標のまま描いています。
- 発生条件: プロキシモードがオンで、Ready のプロキシを持つクリップにブラシがある。
- 影響: プレビューのブラシが、相対的に大きく、ずれた位置に出ます。README には roto と 3D テキストのプロキシ差は書かれていますが、ブラシについては記載がありません。
- 確度: 確認済みです(コード上で確認。調査エージェントが追跡しました)。
- 修正方針: ブラシ経路では元の素材でデコードする。または、ネイティブ解像度との比で座標とサイズを縮尺する。

### [Medium] MCP の `set_color_correction` / `apply_lut` がトラックのロックを無視する
- 場所:
  - src/mcp/McpEditorTools.cpp:4022-4051 → src/Timeline.cpp:11453-11469
  - src/mcp/McpEditorTools.cpp:4092-4096 → src/Timeline.cpp:11471-11495
- 問題:
  - ハンドラにも Timeline 側にも `isLocked()` の検査がありません。他の書き込み系ツールはロック中を拒否しています。
  - 値が変わらなくても、毎回 undo 点を積みます。
- 発生条件: `set_track_locked {"locked":true}` の後に `set_color_correction {"clipIndex":0,"values":{"brightness":50}}` を呼ぶ。
- 影響: ロックで守っているはずのトラックの色や LUT が変わります。
- 確度: 確認済みです(調査エージェントが追跡しました)。
- 修正方針: `readVideoClipTarget(..., forWrite=true)` を使ってロック中を拒否する。値が同じときは undo 点を作らない。

### [Medium] MCP の `set_clip_property` で speed を変えると、ロック中のリンク相手を黙って飛ばし、`linkedApplied:true` を返す
- 場所:
  - src/Timeline.cpp:7670(`partner->isLocked()` なら continue)
  - src/mcp/McpEditorTools.cpp:4178(`linkedApplied` を常に true で返す)
- 問題: 一部にしか適用されていないのに成功を返します。`reversed` では同じ状況で全体を拒否しており、扱いが揃っていません。
- 発生条件: A1 をロックし、V1 と A1 がリンクしている状態で speed=2 を指定する。
- 影響: 映像と音声の尺がずれます。エージェントはずれに気づけません。
- 確度: 確認済みです(調査エージェントが追跡しました)。
- 修正方針: 変更する前にロック中の相手を探し、見つかったら全体を拒否する。

### [Medium] string キーフレームしか持たないクリップは、保存するとそのキーフレームが消える
- 場所: src/ProjectFile.cpp:2072-2074(`tracks` が空でないときだけ `keyframes` を書く)
- 問題: `KeyframeManager::toJson` は `stringTracks` も出力しますが(src/Keyframe.cpp:660)、数値トラックが空だと丸ごと書かれません。
- 発生条件: 数値キーフレームのないクリップに、source_text の string キーフレームを付けて保存する。
- 影響: 開き直すとテキストのアニメーションが消えます。
- 確度: 確認済みです(条件式と `toJson` の両方を読み直しました)。
- 修正方針: 書き出す条件に「`stringTracks` が空でない」も加える。

### [Medium] 自動保存の書き込みがアトミックでなく、失敗しても古いバックアップを消す
- 場所: src/AutoSave.cpp:68-74
- 問題: `QFile` に直接書いており、`write` の戻り値も見ていません。そのまま `cleanOldBackups()` と `autoSaved` の通知を実行します。なお、本体の保存(`ProjectFile::save`)は `QSaveFile` を使っています。
- 発生条件: ディスクの残り容量が足りない、または書き込みの途中でクラッシュする。
- 影響: 途中で切れたファイルが最新の復元候補になり、読み込めません。しかも、正常だった古いバックアップは削除済みです。
- 確度: 確認済みです(コードを読み直しました。wave18 以前からのコードです)。
- 修正方針: `QSaveFile` に切り替え、`commit()` が成功したときだけ cleanup と通知を行う。

### [Medium] MCP の `set_clip_wiggle` が数値の範囲を検査しない(GUI には範囲がある)
- 場所: src/mcp/McpEditorTools.cpp:784-825。比較対象の GUI 側は src/MainWindow.cpp:15695-15711。
- 問題: 有限かどうかしか検査していません。そのため 1e300 や負の周波数も通ります。WiggleTransform.cpp:57 の `static_cast<int>(floor(t*freq))` は、値が int に収まらないと未定義動作になります。
- 発生条件: `{"wiggle":{"enabled":true,"scaleAmplitude":1e300,"positionFrequency":1e300}}`
- 影響: クリップが消えたり反転したりし、その値がプロジェクトに保存されます。
- 確度: 範囲検査がないことは確認済みです。描画にどう影響するかは「可能性あり」です。
- 修正方針: GUI と同じ範囲で検証し、範囲外は拒否する。

### [Low] MCP の `disconnect_clip` がロック中のトラックのクリップの接続を解除できる
- 場所: src/mcp/McpEditorTools.cpp:3828 → src/Timeline.cpp:16005-16035
- 問題: 接続する側の `applyConnection` はロック中を拒否しますが、解除する側には検査がありません。
- 発生条件: V2 をロックした状態で `disconnect_clip {"trackIndex":1,"clipIndex":0}` を呼ぶ。
- 影響: ロック中の構成が変わります。undo では戻せます。
- 確度: 確認済みです(調査エージェントが追跡しました)。
- 修正方針: 解除の対象になるメンバーにロック中のトラックがあれば拒否する。

### [Low] スマートレンダーの丸ごとコピーの判定に、逆再生とタイムリマップの除外がない
- 場所: src/SmartRender.cpp:677-682(`hasSpeedChange` は `speed` / `speedRamp` しか見ていない)
- 問題: `reversed` と `hasTimeRemap()` のクリップもストリームコピーの対象になります。
- 発生条件: `VEDITOR_SMART_RENDER=1`(オプトイン)で、V1 にフル尺で in=0 のクリップが 1 本だけあり、それが reversed か、タイムリマップを持つ。
- 影響: プレビューでは逆再生やリマップされているのに、書き出しは順再生の元素材になります。
- 確度: 確認済みです(判定コードに該当の検査がないことを確かめました)。オプトイン機能なので、影響は限られます。
- 修正方針: `canStreamCopy` の拒否条件に、reversed / タイムリマップ / clipEq / channelMode を足す。

### [Low] MCP の `export_video` に大きな `videoBitrate` を渡すと int が溢れ、極端に低いビットレートになる
- 場所:
  - src/mcp/McpEditorTools.cpp:2889(`static_cast<qint64>(videoBitrate) * 1000`)
  - src/RenderQueue.h:46(代入先が `int bitrateBps`)
- 発生条件: `videoBitrate` に 2147484 以上を渡す(例: 4294968 で 704bps になる)。
- 影響: 「成功」と返るのに、画質が破綻した動画ができます。
- 確度: 確認済みです(型を確かめました)。
- 修正方針: 上限を設けるか、`bitrateBps` を qint64 にする。

### [Low] 保存形式の version が 2 のままで、旧ビルドで開くと 3D テキスト・ブラシ・wiggle・式が黙って消える
- 場所: src/ProjectFile.cpp:298(`PROJECT_FORMAT_VERSION = 2`)
- 問題: データの置き場所をクリップへ移したのに、version を上げていません。ローダーは未知の version も受け入れます。
- 発生条件: wave18 で保存したファイルを旧ビルドで開いて、上書き保存する。
- 影響: クリップに移したデータが永久に失われます。
- 確度: 確認済みです(調査エージェントが差分で確認しました)。
- 修正方針: version を上げる。読み込み時、自分より新しい version なら警告する。

### [Low] 手で編集したプロジェクトの `speed` を検証しない(0 だと尺が無限大になる)
- 場所:
  - src/ProjectFile.cpp:2128(`clip.speed = obj["speed"].toDouble(1.0)`)
  - src/Timeline.h:410-413(`(out - inPoint) / speed`)
- 発生条件: `"speed": 0` や負の値を含むファイルを読み込む。
- 影響: タイムラインの長さが inf になったり負になったりして、描画や書き出しが破綻します。
- 確度: 確認済みです(コード上で確認。実行はしていません)。
- 修正方針: 読み込み時に有限かどうかを検査し、`qBound` で範囲に収める。

### [Low] 音量エンベロープを読み込むときのソートが安定でない
- 場所: src/ProjectFile.cpp:2184(`std::sort`)
- 問題: 書き出し側は wave18 で `stable_sort` に変わりましたが、読み込み側は変わっていません。
- 発生条件: 同じ時刻に 2 点がある(ステップ状の)エンベロープ。
- 影響: 保存→読込でステップの向きが逆になることがあります。
- 確度: 可能性ありです。同じ時刻の点を作る UI の経路は特定できていません。
- 修正方針: `std::stable_sort` にする。

### [Low] MCP の時刻・解像度の入力に上限がない
- 場所:
  - src/mcp/McpEditorTools.cpp:3672-3678(`move_clip` の `newStartSec`)
  - src/mcp/McpEditorTools.cpp:2802-2809(`export_video` の width/height)
- 発生条件: `newStartSec: 1e15`、または `width/height: 60000`。
- 影響: タイムラインの描画でピクセル位置を int に変換するときに溢れたり(Timeline.cpp:321 付近)、巨大なメモリを確保しようとしたりする可能性があります。
- 確度: 可能性ありです。上限がないことは確認しましたが、実際にどう壊れるかは確かめていません。
- 修正方針: 開始秒は 24 時間以下、解像度は 16384 以下などに制限する。

### [Low] MCP の入力と応答の細かな不整合
- 場所:
  - src/mcp/McpEditorTools.cpp:1135-1187(`get_timeline` が `is3DLayer` を返さない)
  - src/mcp/McpEditorTools.cpp:4223-4238(is3DLayer を切り替えると `setClipMotion` が 2D の値をクランプする)
  - src/mcp/McpEditorTools.cpp:4313-4321(`set_clip_expression` が構文エラーの式でも `ok:true` を返す)
- 問題:
  - worldX/Y を使うには is3DLayer=true が前提だと案内しているのに、`get_timeline` ではその現在値が分かりません。
  - 範囲外の 2D 値を持つクリップで 3D を切り替えると、無関係な値が変わります。
  - 誤った式が描画時に黙って無視されるため、エージェントは誤りに気づけません。
- 確度: 返す項目がないことと検証がないことは確認済みです。クランプの影響は「可能性あり」です。
- 修正方針: `is3DLayer` を返す。3D のフィールドだけを書き換える setter を使う。式は `validate` を通らなければ拒否する。

> 補足(報告から除外): split の右側の片で wiggle と式の時刻がずれない件は、README.md:687 に「Wave 19 へ送ったもの」として既知の制限と書かれているため、指摘には含めていません。

---

## 優先して直すべき上位 5 件
1. AI チャットから MCP の `save_project` / `export_video` で任意のパスに書ける件(セキュリティ。拡張子と書き込み先の制限)
2. 式パーサの再帰に深さ上限がない件(保存されたプロジェクトでクラッシュが続く可能性)
3. シーケンスを切り替えると、位置キーの sidecar が別シーケンスに付く件
4. 非アクティブなシーケンスと renderInPlaceOriginal で、タイムリマップ曲線が保存時に失われる件
5. 未保存の変更を確認しない件と、自動保存の復元が止まる件(あわせて AutoSave のアトミック化)

## 調べられなかった範囲とその理由
- ビルドと selftest(ほぼすべて)。依存ライブラリのバージョンが合わず、リンクまで到達できませんでした(下記)。そのため、どの指摘も実行しての再現はしていません。
- GPU の多トラック合成(`src/playback/GpuLayerCompositor.cpp`、VideoPlayer の `composeMultiTrackFrame`)と書き出しの順序・ブレンドの全面比較。HDR16 / ODT、モーションブラー、ネストしたシーケンスの書き出し。コード量が多く、時間内に追いきれませんでした。
- ExportRange.cpp の境界の詳細、Exporter.cpp の旧経路、トラックマットが効いているフレームでの特殊合成のインデックス空間。
- 次の MCP ツールは見ていません: trim / split / delete / transition / 字幕系 / replace / relink / run_command / match_frame / compare_project。`import_edl` / `import_fcpxml` / `open_project` のパーサ内部(巨大な入力、XML のエンティティ展開)と、HTTP サーバの chunked / keep-alive の処理も見ていません。
- `applySidecarState` / `rekeyPositionKeyedSidecars` の uid 対応付けの全経路。Windows で日本語パスを使ったときの実際の動作(コード上は QString / QFile で統一されており、`toLocal8Bit` は見当たりませんでした)。
- リポジトリには `AGENTS.md` と `graphify-out/` がありませんでした。CLAUDE.md が指示している graphify の参照はできず、README とコードを直接読みました。
- 「1 回目の実行後に残高を確認」について: この環境には API の残高や消費額を確認する手段がないため、確認していません。今回は、重点 3 領域を 1 回で並列に調べる範囲にとどめました。残高は Anthropic Console の Usage/Billing などで確認してください(要確認)。

## 実行したビルド・テストのコマンドと結果
- 依存の導入: `apt-get install qt6-base-dev qt6-multimedia-dev libqt6opengl6-dev libavformat-dev libavcodec-dev libavfilter-dev libswscale-dev libswresample-dev` は成功しました(Ubuntu noble: Qt 6.4.2、FFmpeg 6.1)。
- 構成: `cmake -B /home/user/build18 -S . -DCMAKE_BUILD_TYPE=Release` は成功しました。
- ビルド: `cmake --build /home/user/build18 -j -- -k` は失敗しました(rc=2)。46 個の翻訳単位がエラーになり、リンクまで到達していません。原因は次のとおりで、どれも環境のライブラリが古いことによるものです。コードの不具合ではありません。
  - FFmpeg 7.1 以降の API(`avcodec_get_supported_config` / `AV_CODEC_CONFIG_PIX_FORMAT`)が無い: libavcore/Probe.cpp, Encode.cpp, VideoFilterGraph.cpp ほか
  - Qt 6.4 の QPointer / QWeakPointer の API との非互換: AiChatDock.cpp ほか約 35 件
  - QtSvg が未導入: AffinityVectorImporter.cpp
  - 上記に当たらない残りの翻訳単位は、コンパイルが通りました。
- selftest(`--selftest=mcp` など): 実行ファイルを作れなかったため、実行していません。
- 補助の計算: 24/48/12fps でフレーム時刻を切り捨てたときにずれる件数を Python で数えました(結果は本文の Medium の指摘に記載)。
