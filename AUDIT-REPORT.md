# AUDIT-REPORT: v-simple-editor `feat/nle-gap-wave18`(f483e21)

## 全体所見
- 重点 3 領域の監査(第 1 部、23 件)の後、リポジトリ全体(アプリ本体 804 ファイル・25.7 万行とスクリプト/CMake)を 19 区画で全行精読し、さらに cppcheck で機械走査しました(cppcheck は本報告の時点で未完了。末尾参照)(第 2 部、重複統合後 208 件: High 11 / Medium 101 / Low 96)。Critical はありません。
- 最も多い不具合の型は「操作対象の取り違え」です。V1 固定の単一引数 API(`m_videoTrack`)や「V1 と同番号の A1」を書き換える経路が、エフェクトコントロール・カラーパネル・速度・音量などに散在し、V2 以上のクリップの編集が保存されない、または別のクリップを上書きします。
- 次いで「保存/書き出しに届かない状態」(マーカー、スタビライズ結果、カラーパネルの一部、VFX コントロール、マスターコンプ)と、「成功と表示して何もしない機能」が多く、利用者が気付かないまま作業結果を失います。
- 安全性では、MCP/AI チャット経由の任意パス書き込み、Vault を通らない配信系トークン、認証のないネットワークレンダーサーバ、API キーのエラー表示が要対応です。
- ビルドは環境の FFmpeg/Qt が古く通らず、すべて静的追跡による結果です(行番号は f483e21 時点)。

---

# 第 1 部: 重点領域(書き出し/プレビュー不一致・undo/保存・MCP 入力検証)

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

# 第 2 部: リポジトリ全体の監査(重大度順)

各指摘の「区画」は担当した精読区画です(区画とファイルの対応は末尾のカバレッジ表を参照)。他区画の同旨指摘は「統合メモ」に記録し、重複分は削除しました。High の全件と一部の Medium は、私(統合者)がコードを読み直して確認しています(確認した項目: エフェクトコントロール/カラーパネルの V1 固定、1D LUT、マーカー未保存、UniqueConnection、マルチカム同期、テキストベース編集のリップル、V1 の隙間で書き出し失敗、Vimeo キャンセル、crf とビットレート)。

### [High] エフェクトコントロールでの編集が「V1 の選択クリップ」に書かれ、V2 以上のクリップでは保存されない（または別クリップを上書きする）
- 区画: part03
- 場所: src/EffectControlsPanel.cpp:997-1019, 1053, 1065, 1150, 1190, 1214, 1315, 1322 / src/MainWindow.cpp:7217-7220 / src/Timeline.cpp:11891-11899, 11922-11930
- 問題: パネルは `m_currentClipKey`（clipSelectedOnTrack で受けた任意トラック）を対象に表示するが、パラメータ変更・バイパス・リセット・エフェクト追加は `effectsChanged` → MainWindow のラムダ → `Timeline::setClipEffects(effects)`、キーフレームの作成/削除/ナビバー編集は `Timeline::setClipKeyframes(km)` を呼ぶ。どちらも `m_videoTrack`（常に V1。Timeline.cpp:5367）の `selectedClip()` を対象にする単一引数版で、`m_currentClipKey.trackIdx` を使わない。
- 発生条件: V2 以上のクリップを選択してエフェクトコントロールでパラメータを動かす / エフェクトを追加する / キーフレームを付ける。通常クリックでは他トラックの選択が外れる（Timeline.cpp:12926-12933）ので V1 の selectedClip は -1。
- 影響: 編集が timeline に保存されず undo 点も作られない（パネル表示だけ変わり、再選択で元に戻る）。リンク選択や Ctrl 追加選択で V1 にも選択クリップがあると、V2 のエフェクト列/キーフレームで V1 のクリップを上書きする（データ破壊）。
- 確度: 確認済み（panel → MainWindow:7217 → Timeline::setClipEffects(m_videoTrack) を追跡）
- 修正方針: `setClipEffectsAndKeyframes(m_currentClipKey.trackIdx, clipIdx, ...)` / `setClipKeyframes(TrackKind::Video, trackIdx, clipIdx, km)` のトラック指定版に統一し、effectsChanged ハンドラも (track, clip) を受け取る。

### [High] 1D の .cube LUT を選ぶと、GPU プレビューが 3D テクスチャとして N^3 要素を読み、バッファ外を読んで落ちる
- 区画: part04
- 場所: src/GLPreview.cpp:3711-3742(`GLPreview::setLut`)、src/LutImporter.h:27(`isValid`)、src/LutImporter.cpp:59-63 / :92-96、呼び出し元 src/MainWindow.cpp:13588-13599 / :6955-6965
- 問題:
  - `LutImporter::loadCubeFile` は `LUT_1D_SIZE N` の .cube を `type = Cube1D`、`table.size() == N` の有効な LUT として返します(`isValid()` は `size > 0 && !table.isEmpty()` だけを見ます)。
  - `GLPreview::setLut` は `lut.type` を見ずに `setSize(N, N, N)` の RGB32F 3D テクスチャを作り、`setData` に N*3 個しかない float 配列を渡します。ドライバは N^3*3 個の float を読みます。
  - CPU 側(`LutImporter::applyLut`)は 1D を正しく扱うので、書き出しは動き、プレビューだけが壊れます。
- 発生条件: 「LUT を読み込み」/「Apply LUT」で `LUT_1D_SIZE` を持つ .cube(1D の LUT は一般に配布されています)を選ぶ。LUT ライブラリに登録された 1D LUT をカラーパネルで選ぶ場合も同じです。
- 影響: ヒープのバッファ外読み取りによるクラッシュ、またはごみの色。N=1024 では 1024^3 の RGB32F(約 12 GB)の確保を要求し、確保の失敗やドライバの異常終了になります。
- 確度: 確認済み(loadCubeFile → applyLutFileToSelectedClip → GLPreview::setLut の経路と、setSize / setData の要素数を確かめました。実機では動かしていません)
- 修正方針: `setLut` で `lut.type == Cube1D` のときは 1D を 3D に展開して(各軸を 1D 表で引いた size^3 の表を作って)渡すか、GPU の LUT を無効にして CPU 経路に回す。あわせて `table.size() == size^3` を確かめてから `setData` する。

### [High] カラーパネルのビネット / Hue vs Sat / クロマキー / マスクは GLPreview の uniform にしか入らず、クリップにも保存にも書き出しにも残らない
- 区画: part04
- 統合メモ: part02 の同旨指摘(Sharpen/Blur/Lens・3D 回転・エフェクトパックも同様)を統合。
- 場所: src/GLPreview.cpp:3826-3900(`setVignette` / `setChromaKey` / `setMask`)、:3809-3820(`setHueVsSatLut`)、:2551-2585(paintGL で毎回送る)、配線 src/MainWindow.cpp:7056-7098
- 問題:
  - MainWindow の `vignetteChanged` / `hueVsSatChanged` / `chromaKeyChanged` / `maskChanged` のハンドラは `m_player->glPreview()->setXxx(...)` を呼ぶだけです。選択中のクリップにも Timeline にも書きません。
  - GLPreview はこれらをウィジェット全体の状態として持つため、クリップを切り替えても、別のクリップや別のトラックの絵に同じ効果が掛かり続けます。
  - 書き出し(tlrender::renderFrameAt)やプロジェクトの保存は、これらの値を知りません。
- 発生条件: カラーパネルでビネット、Hue vs Sat カーブ、パネルのクロマキー、パネルのマスクのどれかを操作する。
- 影響: プレビューでは効果が見えるのに、書き出した動画には入りません。保存して開き直すと消えます。編集していないクリップのプレビューにも掛かります。
- 確度: 確認済み(4 つのシグナルの接続先がこのハンドラだけであることを grep で確かめました。ColorGradingPanel にも値を保存する経路はありません)
- 修正方針: 値を ClipInfo(の colorCorrection など)に持たせて undo / 保存 / 書き出し(CPU SSOT)に通し、GLPreview はクリップの値から uniform を作る。すぐに対応できないなら、UI に「プレビューのみ」と明示する。

### [High] タイムラインのマーカーがプロジェクトに保存されず、別プロジェクトを開いても前のマーカーが残る
- 区画: part06
- 場所: src/MainWindow.cpp:9024-9198 (populateProjectData にマーカーが無い)、src/MainWindow.cpp:9248-9492 (applyLoadedProjectData で setMarkers / クリアを呼ばない)、src/ProjectFile.cpp:793 / :1423 (`root["markers"] = QJsonArray{}` 固定)、src/Timeline.cpp:17193 (`Timeline::setMarkers` は本番コードから呼ばれない)、src/MainWindow.cpp:14403-14463 (Ctrl+M の addMarker は別系統の MarkerManager)
- 問題: M / Shift+M / シーン検出 / マーカーパネルで扱う `Timeline::m_markersData` は、保存時に常に空配列で書かれ、読込時も復元されない。読込・新規作成でクリアもされない。さらに「再生ヘッドにマーカー追加」(Ctrl+M、ツールバーの「マーカー」) は旧 `MarkerManager` に入り、マーカーパネル・保存・undo のどれにも乗らない (チャプター書き出しだけがこちらを読む)。
- 発生条件: マーカーを付けて保存し、開き直す。または A を開いてマーカーを付け、B を開く。
- 影響: ユーザーのマーカー・ノート・期間マーカーが保存後に全て消える (README の「Timeline markers & YouTube chapter export」機能が実質的に永続化されない)。B を開くと A のマーカーが B に表示され、B を保存しても残らない。Ctrl+M のマーカーはパネルに出ず、チャプター書き出し以外から見えない。
- 確度: 確認済み (setMarkers / projectFileMarkersToJson の呼び出し元を grep。selftest 以外に無い。ProjectFile の save 両経路で空配列固定)
- 修正方針: ProjectData に markers を追加し projectFileMarkersToJson/FromJson で往復、applyLoadedProjectData と newProject で `setMarkers` (空含む) を呼ぶ。Ctrl+M も Timeline::addMarker に統一し、チャプター書き出しも Timeline マーカーから作る。

### [High] トラッキング / スタビライズを 2 回目に実行すると、解放済みの進捗ダイアログに触れてクラッシュする (UniqueConnection がラムダに効かない)
- 区画: part06
- 統合メモ: part07 の同種指摘(モーショントラッキング、MainWindow.cpp:12988 付近)を統合。同じ原因(ラムダ + Qt::UniqueConnection)は字幕生成(part10)・ノイズ除去などにもある。
- 場所: src/MainWindow.cpp:12982-12992, :13067 (trackMotion)、src/MainWindow.cpp:13413-13420 (stabilizeVideo)。同じ型: :13103-13113 (motionTrackSetup)、:13150-13159 / :13201-13210 (denoise)、:13245-13292 (generateSubtitles)、:14315-14320、:14356、:14395、:14499-14507、:14542-14570
- 問題: `connect(sender, signal, this, [ ... ], Qt::UniqueConnection)` はラムダ (関数オブジェクト) では重複判定されない (Qt の仕様。slot がメンバ関数ポインタのときだけ比較する)。そのため呼ぶたびに `m_motionTracker` / `m_stabilizer` などの長寿命オブジェクトへ同じハンドラが積み増される。trackMotion と stabilizeVideo のハンドラは生ポインタ `progress` を捕捉しており、1 回目の完了で `progress->deleteLater()` 済み。
- 発生条件: 「モーショントラッキング」(または「スタビライズ」) を 1 回完了させたあと、もう一度実行して完了させる。
- 影響: 2 回目の完了シグナルで 1 回目のラムダが解放済みの QProgressDialog に `close()` を呼ぶ (use-after-free → クラッシュ)。生き残った場合も適用先選択ダイアログや「字幕をどうするか」ダイアログが回数分出て、SRT 書き出し・オーバーレイ適用が重複する。trackMotion と motionTrackSetup は同じ m_motionTracker を共有するため、互いのハンドラも発火する。
- 確度: 確認済み (ラムダ接続・context=this・progress の deleteLater を追跡。UniqueConnection がラムダに効かないのは Qt のドキュメント記載どおり)
- 修正方針: 接続を QMetaObject::Connection に保持して実行前に disconnect するか、context を progress にする / Qt::SingleShotConnection を使う。

### [High] マルチカムの音声同期 (estimateOffsetMs) が重なり 1 サンプルの端のラグを常に最良と判定し、実素材ではほぼ必ず誤ったオフセットを返す
- 区画: part07
- 場所: src/MultiCamSync.cpp:82-123 (呼び出し: src/MultiCamSync.cpp:34 estimateOffsetsForFiles → src/MultiCamDialog.cpp:308 onSync、src/MultiCam.cpp:65 autoSyncByAudio)
- 問題: ラグ探索範囲が ±(min(nRef,nOther)-1) で、重なりの長さに下限が無い。lag = -maxLag では overlap = 1 になり、正規化相関は a*b / (|a|*|b|) = 1.0 (envelope は非負のピーク値なので a,b>0 なら厳密に 1.0) になる。ループは lag=-maxLag から始まり、`corr > bestCorr` の厳密比較のため、以降の本当の整列ラグ (Cauchy-Schwarz で ≤1.0) はこれを超えられない。overlap 2〜3 の端のラグも 1.0 近くになる。
- 発生条件: 「音声で同期」(MultiCamDialog) / MultiCamSession::autoSyncByAudio で、基準アングルの最後の 20ms 区間と比較アングルの最初の 20ms 区間がどちらも無音でない (環境音があれば常に該当)。selftest (sprint_selftests.cpp:1617-1700) は両端を 0 で埋めた合成データのみで、この経路を通らない。
- 影響: オフセットが約 -(短い方の尺 - 20ms) (例: 10 分の素材なら約 -600 秒) になり、マルチカムの同期結果が全く使えない。加えて O(N^2) (1 時間素材で約 1.3e10 回の積和) を GUI スレッドで同期実行するため長時間フリーズする。
- 確度: 確認済み (lag=-maxLag の overlap=1 で corr=1.0 になることを式で追跡)
- 修正方針: 最小重なり (例: 短い方の 50% または数秒分) と最大ラグを設け、平均を引いた相関 (ZNCC) または重なり長で重み付けする。FFT 相関に置き換えワーカースレッドで実行する。

### [High] 書き出しのワーカースレッドが、編集中のライブ Timeline をロックなしで毎フレーム読む
- 区画: part09
- 統合メモ: part13 の同旨指摘(書き出しスレッドの同期なし読み取り)を統合。
- 場所: src/RenderQueue.cpp:1458,1795(in-process 経路)/ src/RenderQueue.cpp:2016,2295(HDR サブプロセス経路)/ src/SequenceExport.cpp:109 / src/RenderQueueDialog.cpp:453 / 呼び出し側 src/MainWindow.cpp:11133
- 問題: `job.timeline` に MainWindow のライブ `m_timeline` を入れたジョブでは、`resolveTimeline` がそのポインタをそのまま返します。その後 `QThread::create` のワーカーが `tlrender::renderFrameAt(tl, …)` を毎フレーム呼びます。renderFrameAt は `timeline->activeSequenceId()`、`projectLights()`、`sequences()`、トラックやクリップのベクタを毎回読みにいきます(TimelineFrameRenderer.cpp:3347 付近)。GUI スレッド側の編集との間に、ロックもスナップショットもありません。
- 発生条件: (1) RenderQueueDialog(非モーダル、`setModal(false)`)で入力欄を空のままジョブを追加し、レンダー中にタイムラインを編集する。(2) 通常の書き出しは WindowModal の進捗ダイアログで手入力を防いでいますが、その間も MCP のツール呼び出しは GUI スレッドでタイムラインを変更できます。MCP 側には書き出し中かどうかの確認がありません。
- 影響: QVector / QHash の detach や再割り当てと、別スレッドからの読み取りが競合します。その結果、クラッシュ、壊れたフレーム、編集途中の状態が混ざった書き出しが起こります。
- 確度: 確認済み(同期のない共有アクセスを、呼び出し元からワーカーまで追跡。クラッシュするかどうかはタイミング次第)
- 修正方針: ジョブ開始時に GUI スレッドで Timeline を複製し(resolveTimeline の file 経路と同じく、所有する別インスタンスにする)、それを渡す。あるいは、書き出し中は編集と MCP の変更系ツールを拒否する。

### [High] テキストベース編集の「リップル削除」がアクティブな映像トラック 1 本だけを詰め、音声トラックを詰めない(ロックも無視し、区間ごとに undo が積まれる)
- 区画: part11
- 場所: src/TextBasedEditDialog.cpp:195 (applyDeletions emit) → src/MainWindow.cpp:17505-17523 (受け手) → src/Timeline.cpp:12135-12161 (Timeline::rippleDeleteTimeRangeActive)
- 問題: ダイアログが出す削除区間列を、MainWindow は区間ごとに `rippleDeleteTimeRangeActive` へ渡す。この関数はアクティブ映像トラック(無ければ V1)の 1 本だけを `rippleDeleteTimeRange` し、音声トラック・他の映像トラックには触れない。トラックのロックも見ない(`isLocked` の検査が無い)。さらに区間ごとに `saveUndoState("リップル削除 (範囲)")` を呼ぶので、N 区間なら undo が N 段積まれる。全トラックをロック考慮で一括処理し undo 1 段にする `Timeline::applyRippleDeleteTimeRangesToAllTracks`(Timeline.cpp:8590)が既にあるのに使っていない。適用後に字幕エディタのトラック(m_captionEditorDialog->track())の時刻もずらさないため、もう一度テキストベース編集を開くと古い時刻で別の場所を削除する。
- 発生条件: 文字起こし後に「テキストベース編集」で 1 行以上チェックして「タイムラインに適用 (リップル削除)」。映像と音声が別トラック(通常の取り込み)なら必ず起きる。
- 影響: 最初の削除点より後ろで映像と音声(文字起こしの元の音声)がずれ、削除したはずの発話が音声トラックに残る。ロック中のトラックも詰められる。取り消しは区間数ぶん Ctrl+Z が要る。2 回目以降の適用は誤った範囲を消す。
- 確度: 確認済み(ダイアログ→MainWindow のラムダ→rippleDeleteTimeRangeActive→TimelineTrack::rippleDeleteTimeRange をたどった。どこにもロック検査・音声トラック処理が無い)
- 修正方針: ラムダで区間を TimeRangeSec に変換し、`applyRippleDeleteTimeRangesToAllTracks(ranges, skipLocked=true, "テキストベース編集")` を 1 回呼ぶ。適用後は字幕トラックの時刻も同じ区間で詰めるか、ダイアログの再利用時に再文字起こしを促す。

### [High] V1 に隙間がある、または上位トラックが V1 より長いタイムラインは、通常の書き出しが途中で失敗する
- 区画: part13
- 統合メモ: README.md:741(Wave 14、利用者判断 3)に「V1 の隙間区間の絵は変わらない」とあるが、書き出し自体の失敗には触れていないため指摘として残す。既知扱いか要確認。
- 場所: src/TimelineFrameRenderer.cpp:2507-2509 (v1Idx < 0 で return QImage())、src/RenderQueue.cpp:1795-1801 / 2295-2304 (null フレームでジョブ失敗)、src/Timeline.cpp:12772-12786 (totalDuration は全映像トラックの最長)
- 問題: renderFrameFromTracks は「V1 が表示中でクリップを持つが、その時刻に V1 のクリップが無い」とき、上位トラックを集める前に空の QImage を返す。RenderQueue の描画ループ (File→Export は submitNormalExport からここを通る) は null フレームを受けると "renderFrameAt returned a null image at frame N" でジョブ全体を失敗させる。書き出し尺は totalDuration() = 全映像トラックの最長なので、V1 の後ろに V2 だけが続く区間でも同じ。
- 発生条件: ストーリーラインモード OFF で V1 クリップを削除・リフトして隙間が残る / V1 の 2 本目以降に leadInSec がある / V2 (タイトル・BGM 映像など) が V1 の末尾より長い。いずれも普通の編集で起きる。
- 影響: 書き出しが隙間のフレームで中断し、出力ファイルが得られない (MCP export_video も同じ RenderQueue 経路)。README 741 行は「V1 にクリップがあって隙間のある区間の絵は変わりません (利用者判断 3)」とだけ書いており、書き出しそのものが失敗することは書かれていない。
- 確度: 確認済み (renderFrameAt → renderFrameAtSingle → renderFrameFromTracks:2507 → RenderQueue:1795 を追跡。brush_export_selftest:964 も「V1 の隙間では renderFrameAt が null」を前提にしている)
- 修正方針: 隙間では透明 (書き出し時は黒) の下地を返して上位トラックを合成するか、少なくとも RenderQueue 側で null を黒フレームに置き換えて続行する。利用者判断 3 が「隙間は描かない」の意味なら、その場合も黒フレームを出して書き出しは止めない。

### [High] Vimeo のアップロード中に「Cancel Job」を押すと null の QPointer に deleteLater して落ちる
- 区画: part15
- 場所: src/VimeoUploadClient.cpp:451-458 (clearReply)、呼び出し元 :135-140 (cancel)、src/VimeoUploadManager.cpp:40-43 (cancelJob)、src/VimeoUploadDialog.cpp:174-190
- 問題: clearReply(QPointer<QNetworkReply>& reply) は `reply->abort()` の直後に `reply->deleteLater()` を呼ぶ。QNetworkReply::abort() は未完了の reply に対して finished() を同期で emit する (QNetworkReplyHttpImpl::abort → d->finished())。finished は同じスレッドの直接接続で onCreateUploadFinished / onChunkReplyFinished / onCompleteReplyFinished に入り、各スロットは先頭で `m_xxxReply.clear()` する。clearReply の引数 `reply` はそのメンバへの参照なので、abort から戻った時点で null になっており、続く `reply->deleteLater()` は nullptr に対するメンバ呼び出しになる。
- 発生条件: アップロード中 (create / tus PATCH / 完了確認 HEAD のいずれかが飛んでいる間) にダイアログで行を選んで「Cancel Job」を押す。startUpload の先頭で cancel() を呼ぶ経路も同じ。
- 影響: アプリがクラッシュする (編集中のプロジェクトの未保存分を失う)。加えて abort で走ったスロットが scheduleRetry を呼び、リトライタイマーも積まれる (resetJobState が phase を None に戻すので発火はしない)。
- 確度: 確認済み (cancel → clearReply → abort → 同期 finished → スロットで QPointer を clear → null に deleteLater を追跡。Qt の abort が finished を同期 emit する点は Qt のソースどおり)
- 修正方針: clearReply で `QNetworkReply *r = reply.data(); reply.clear(); r->disconnect(this); r->abort(); r->deleteLater();` の順にする (先にポインタを退避して切断してから abort)。

### [High] カラーグレーディングパネルの基本補正・ホイール・WB・リセットが V1 の選択クリップにしか書かれず、V2 以上のクリップではプレビューだけ変わって保存も書き出しもされない
- 区画: part19
- 場所: src/MainWindow.cpp:6867-6874 (writeSelectedClipColorCorrection), 6928-6934 (colorCorrectionChanged), 6976-6984 (resetRequested), 6985-6994 (clipSelectedOnTrack での読み込み), 6996-7001 (colorWheelsChanged), 7016-7024 (whiteBalanceChanged) / 呼び先 src/Timeline.cpp:8966-8969 (hasSelection), 11443-11451 (setClipColorCorrection 単一引数版), 11993-11998 (clipColorCorrection)
- 問題: パネル側の書き込みは `m_timeline->hasSelection()` / `clipColorCorrection()` / `setClipColorCorrection(cc)` を使っています。この 3 つはどれも `m_videoTrack` (Timeline.cpp:5367 で常に V1) の `selectedClip()` だけを見ます。クリップ選択時の読み込み (clipSelectedOnTrack ハンドラ) も同じなので、V2 以上のクリップを選ぶと `hasSelection()` が false になり、パネルは前のクリップの値のまま残ります。一方、プレビューへの反映 (`applyColorWheelsToPreview` / `glPreview()->setWhiteBalance`) は選択に関係なく実行されます。同じパネルでも、LUT (6935-6946) とカラーワーパー (6906-6927) は `selectedVideoClipRef` (追跡中のトラック) に、カーブ / HSL は `selectedClipIndex` (選択のある最初のトラック) に書くので、項目ごとに書き込み先の決め方が違います。
- 発生条件: V2 以上のクリップを通常クリックで選び (他トラックの選択は外れる: Timeline.cpp:12926-12933)、カラーグレーディングパネルで露出・彩度・Lift/Gamma/Gain ホイール・ホワイトバランスを動かす、または「すべてリセット」を押す。Ctrl+クリックやリンク選択で V1 にも選択が残っている場合は、V2 を表示したまま編集すると V1 のクリップに書かれます。
- 影響: プレビューでは色が変わって見えるのに、クリップには何も書かれません (undo 点も作られず、保存・書き出しに残らない)。V1 にも選択がある場合は、無関係の V1 クリップの色補正が上書きされます。また、V1→V2 と選択を移してもパネルに V2 の値が読み込まれません。ロック中のトラックかどうかも確認していません。
- 確度: 確認済み (パネルの各シグナル → MainWindow のラムダ → Timeline の単一引数版が m_videoTrack だけを見ることを追跡。V2 選択時に V1 の選択が外れる処理も確認)
- 修正方針: 書き込みと読み込みを `selectedVideoClipRef` (または clipSelectedOnTrack で受けた track/clip) に統一し、`setClipColorCorrection(trackIdx, clipIdx, cc)` を使う。書き込み前にトラックのロックを確認する。

### [Medium] 細工した PSD で範囲外読み出し・巨大確保が起き、取り込みダイアログがクラッシュする
- 区画: part01
- 場所: src/AffinityPsdImporter.cpp:86-99(readUnicodeString)、:437-441(luni を aLen で制限せずに読む)、:467-468 / :494-511(レイヤー境界から w,h を算出)、:221-249(buildLayerImage)、呼び出し元 src/ImportHubDialog.cpp:338
- 問題:
  - `luni` の文字数 `count`(uint32)を検証していません。`static_cast<int>(count) * 2` が溢れます。たとえば count=0x80000000 だと 0 バイトの QByteArray になり、`readBytes(...,0)` は成功します。その後 `for (i < count) buf[i*2]` で範囲外を読み続けます。count=0x3FFFFFFF なら 2GB を確保しようとします。
  - レイヤーの top/left/bottom/right をキャンバスの寸法と突き合わせていません。`int(L.right - L.left)` は qint32 の減算なので溢れます。`buildLayerImage(w,h)` はチャンネルがなくても w×h×4 の QImage を確保して fill します(46340×46340 で約 8.6GB)。`planeBytes = width*height` や `idx = y*width+x` も int で溢れます。確保に失敗して QImage が null になると、`scanLine()` が nullptr を返し、そこへ書き込みます。
- 発生条件: ヘッダ数十バイトの細工した .psd を Import Hub の「Affinity / Photoshop」から開く(サイズの小さいファイルで再現します)。
- 影響: アプリがクラッシュする、またはメモリを使い果たして固まります(信頼できない素材ファイルによる DoS)。
- 確度: 確認済み(count の溢れ → 0 長バッファ → ループでの範囲外読み出し、および境界 → QImage 確保 → null の scanLine という経路をコード上で追いました)
- 修正方針: `count` を `aLen`/2 と残りバイト数で上限チェックし、読み込みは `aStart + aLen` を超えないようにする。レイヤーの w/h は 64bit で計算し、キャンバス寸法(と上限 30000 程度)で拒否する。QImage が null なら空のレイヤーにする。

### [Medium] マスターコンプレッサー/リミッターとラウドネスノーマライザーがプレビューにしか効かず、書き出しに入らない
- 区画: part01
- 場所: src/AudioMixer.cpp:1310-1416(readData でノーマライザーとマスターコンプを適用)、src/AudioExportMix.cpp:224-321 / :329-567(buildMixPlan と executeMixPlan はマスターについて masterChain の EQ / DeEsser しか見ない)
- 問題: readData は合算後に `m_normalizerAmount` の自動ゲインと `m_compressorParams` のコンプ+0.99 リミッターを掛けます。書き出しの MixPlan にはどちらの値もなく、`useMasterEq` も EQ/DeEsser しか判定しません。src 全体を grep しても、compressorParams / normalizerAmount を読むのは MainWindow の保存・読込と UI だけでした。passthrough 判定もこの 2 つを考慮しません。
- 発生条件: 「マスターコンプレッサー設定」を有効にする、または normalizerAmount を 0 より大きくする(プロジェクトに保存されるので、読み込んだ直後も有効)。この状態で書き出す。
- 影響: プレビューで調整した音量やダイナミクスが、書き出し結果と大きく違います。プレビューではリミッターでクリップしていなかった音が、書き出しではクリップすることもあります。README にはこの差についての記載がありません。
- 確度: 確認済み
- 修正方針: MixPlan にマスターコンプとノーマライザーのパラメータを入れ、finishMix の float WAV 段階で同じ処理(MasterEqFilter と同様の共通実装)を掛ける。あわせて passthrough の判定にも加える。

### [Medium] Track EQ プリセット(5〜6 バンド)を 3 バンドのレガシー EQ に入れるため、3 番目の帯は高域シェルフとして解釈され、4 番目以降は捨てられる
- 区画: part01
- 場所: src/AudioMixer.cpp:2499-2524(setTrackEqConfig は `i < 3` だけを処理し、i==2 を calcHighShelf にする)、:576-597(recomputeEqCoefficients も同じ)、src/AudioEQ.cpp:369-429(プリセットは 5〜6 バンド)、呼び出し元 src/MainWindow.cpp:5241-5255 と :20486-20497
- 問題: たとえば "Bass Boost" では {400Hz,+2dB} が 400Hz 以上全体を +2dB 持ち上げる高域シェルフになり、その先の帯は無視されます。"Voice Clarity" / "Podcast" は 3000Hz の +4〜5dB(プリセットの主目的)が捨てられ、3 番目の帯は 1000Hz 0dB なので平坦になります。"Music" は 1000Hz -1dB が高域シェルフになり、4k / 12k のブーストが消えます。オフラインの「Audio Equalizer」(AudioEQProcessor::buildEQFilter)は、同じプリセットの全帯をピーキングとして掛けます。そのため、同じ名前のプリセットでも音が違います。
- 発生条件: オーディオ > Track EQ > A<n> > プリセット、またはメーターの右クリック > EQ プリセットを選ぶ。
- 影響: 選んだプリセットとは別の EQ が掛かります。書き出しも trackEqCoeffs を使うため、同じ誤った音になります。
- 確度: 確認済み
- 修正方針: 3 バンド用のプリセットを別に定義する。または、プリセットを 4 バンドのトラック EQ(trackfx の chain)へ割り当てる。帯数が 3 を超えるときは警告する。

### [Medium] オーディオエフェクト「Fade Out」を掛けると、冒頭 1 秒でフェードアウトし、その後は全部無音になる
- 区画: part01
- 場所: src/AudioEQ.cpp:315-320(`afade=t=out:d=%1`、st なし)、呼び出し元 src/MainWindow.cpp:14391
- 問題: FFmpeg の afade は `st`(start_time)の既定値が 0 です。終端から逆算した st を付けていないため、0 秒から d 秒でフェードアウトし、以降は無音になります。コメントには「caller should compute st」とありますが、呼び出し元はそれを計算していません。
- 発生条件: オーディオ > Audio Effects > 「Fade Out」を選んで保存する。
- 影響: 出力ファイルの音声が 1 秒目以降すべて無音になります(処理自体は成功と表示されます)。
- 確度: 確認済み(afade の st の既定値 0 は FFmpeg の仕様)
- 修正方針: ffprobe や libav で長さを取り、`st = duration - d` を渡す。または `areverse,afade=t=in,areverse` にする。

### [Medium] AI ハイライトリールの書き出しが fps を整数に丸め、PTS を連番で振るため、音ズレが溜まる
- 区画: part01
- 場所: src/AIHighlight.cpp:106-112(fpsFromVideoProps で round)、:838(request.fps に整数を入れ、fpsNum/fpsDen を使わない)、:974(`videoPts++`)、:1024-1028(音声はサンプル数で連続した PTS)
- 問題:
  - 29.97 / 23.976 / 59.94 の素材が 30 / 24 / 60 fps として符号化されます。映像は 0.1% 短くなり、音声はサンプル数どおりの長さになります。
  - VFR(スマートフォンの動画など)のフレームも等間隔の PTS に詰め直すため、さらに大きくずれます。
  - 各ハイライトの境界でも、映像と音声のフレームの切り方が独立しているため、端数が積み重なります。
- 発生条件: NTSC 系や VFR の素材で analyze → 書き出し(リールを長くするほど悪化します。300 秒で約 0.3 秒)。
- 影響: 書き出したリールの後半ほど、口の動きと音が合わなくなります。
- 確度: 確認済み(EncodeRequest には fpsNum/fpsDen があるのに使っていない)
- 修正方針: `videoProps.frameRate` を fpsNum/fpsDen に渡す。映像の PTS は元の pts からハイライトごとのオフセットを引いて算出し、音声と同じ基準にそろえる。

### [Medium] Codex の 2 回目以降のプロンプト(exec resume)には読み取り専用サンドボックスが付かない
- 区画: part01
- 場所: src/AiChatDock.cpp:284-289(`-s read-only` を threadId が空のときだけ付ける)、:682-686(m_sessionId があれば resume)
- 問題: 最初の `codex exec` だけが `-s read-only` で動きます。同じセッションの 2 通目以降は `exec resume <id>` で、sandbox の指定がありません。このとき、ユーザーの ~/.codex/config.toml の `sandbox_mode`(workspace-write や danger-full-access)か、CLI の既定値に従います。代わりに `-c sandbox_mode='read-only'` を渡すこともしていません。
- 発生条件: AI チャットで Codex CLI を選び、2 通目を送る。ユーザー設定の sandbox が read-only 以外になっている。
- 影響: Dock が想定している「エージェントは読み取り専用」という前提が、2 通目から崩れます。プロンプトインジェクションを受けると、プロジェクトのディレクトリ(作業ディレクトリ)への書き込みやコマンド実行ができるようになります。
- 確度: 可能性あり(resume の sandbox が設定ファイル由来になることは、Codex CLI の仕様の記憶に基づいています。コード上で resume に sandbox 指定がないことは確認済みです)
- 修正方針: resume のときも `-c sandbox_mode='read-only'` を常に渡す(-c は resume でも受け付けられる)。

### [Medium] UI で設定や実行を受け付けても何も起きない機能が 3 つある
- 区画: part01
- 場所:
  - src/AudioRestorationDialog.cpp:157-219(Apply を押しても選んだファイルを読まない。1 秒の合成バッファを処理して qWarning を出すだけ)
  - src/AIProcessingDialog.cpp(設定は src/MainWindow.cpp:16141-16149 で m_aiSettings に入り、ProjectFile に保存されるが、レンダーや書き出しのどこからも参照されない。upscaleEnabled / frameInterpEnabled を grep しても MainWindow / ProjectFile 以外に使用箇所はない)
  - src/AIMaskDialog.cpp:202-209(src/MainWindow.cpp:16165-16175 が setSourceImage を呼ばないので、「生成」は常に "source image is null" のエラーになる)
- 問題: どれもメニューから開けて、操作は成功したように見えます(AudioRestoration は何も表示しない。AIProcessing は「AI 処理設定を更新しました」と表示される)。しかし、実際の処理は行われません。README の Sprint 15-22 には audio restoration が完了済み([x])として載っています。
- 発生条件: 音声リストアで Apply を押す、AI 処理設定でアップスケールや補間を ON にして書き出す、AI マスクで生成を押す。
- 影響: ユーザーは処理されたと誤解します(リストアしたつもりのファイルは変わらず、書き出しもアップスケールされません)。
- 確度: 確認済み
- 修正方針: 実装するまでメニュー項目を無効にするか、「未実装」と明示する。リストアは libavcore::extractAudioToWav と readPcm16WavToMono で読み、processAll の結果を書き出す。

### [Medium] カラーホイールのスライダーを 1 本動かすと、範囲外だった他のチャンネルの lift / gamma / gain が黙って丸められる
- 区画: part02
- 場所: src/ColorGradingPanel.cpp:1548-1580 (表示側 1615-1646, 1255-1258, 1238-1248)
- 問題: `onWheelSliderChanged` は 12 本のスライダー全部を読み直して `syncColorCorrectionFromWheels` で m_cc を作り直します。lift/gain スライダーは ±1.0 (±100)、gamma は 0.1..4.0 の範囲しかありません。一方で保存値は「RGB + Luma」を畳み込んだ値です (lift/gain は最大 ±2、gamma は最大 16。MCP の許容範囲も同じ: mcp/McpEditorTools.cpp:316-325)。`setColorCorrection` → `setWheels(wheelsFromColorCorrection())` は Luma を 0/1 に戻して RGB に全部載せるので、スライダーへ書くときにクランプされます。
- 発生条件: 例: Lift R=1.0 と Lift Luma=0.5 を設定 (liftR=1.5 で保存) → 別クリップを選んでから戻る (setColorCorrection) → Gain G など、どれか 1 本のホイールスライダーを動かす。MCP で gainR=1.5 や gammaR=log2(8) を入れた場合も同じです。
- 影響: 触っていないチャンネルの値が 1.0 / 4.0 に書き換わり、グレードが黙って変わります。
- 確度: 確認済み (liftGainToSlider(1.5)=150 → QSlider が 100 にクランプ → sliderToLiftGain=1.0 で m_cc.liftR を上書き)
- 修正方針: 動かしたスライダーの軸だけを m_cc に反映する。または、スライダーの範囲を保存値の範囲に合わせる (Luma を別に保存する)。

### [Medium] カーブ / HSL セカンダリの編集で、マウスを動かすたびに undo が 1 段積まれ、ロック中のトラックにも書き込まれる
- 区画: part02
- 場所: src/ColorGradingPanel.cpp:164-193, 195-220, 1042-1049, 2198 (発火元 src/CurveEditor.cpp:438-473)
- 問題: CurveEditor は `mouseMoveEvent` のたびに `curvesChanged` を出し、パネルはそのたびに `track->setClips` と `saveUndoState("Clip RGB curves")` を呼びます。HSL の各スライダーの valueChanged も同じです。さらに `writeCurvesToSelectedClip` / `writeHslSecondaryToSelectedClip` は `track->isLocked()` を確認していません。
- 発生条件: カーブの点を 1 回ドラッグする / HSL スライダーをドラッグする / ロック中トラックのクリップを選んだままカーブを触る。
- 影響: 1 回のドラッグで数十段の undo が積まれ (Ctrl+Z で 1 マウスイベントずつしか戻らない)、undo の上限があれば古い履歴を押し出します。saveUndoState はタイムライン全体を直列化するので、ドラッグ中に重くなります。ロック中のトラックの色も変わります。
- 確度: 確認済み (CurveEditor::mouseMoveEvent:473 で emit、パネル側 lambda → writeCurvesToSelectedClip → saveUndoState)
- 修正方針: ドラッグ中はプレビューだけ更新し、mouseRelease (または editingFinished / sliderReleased) で 1 回だけクリップ書き込みと undo を行う。書き込み前にトラックのロックを確認する。

### [Medium] ブラシアニメーションをダイアログから追加しても何も表示されない (基準位置が (0,0)、所要時間が使われない)
- 区画: part02
- 場所: src/BrushAnimationDialog.cpp:135 (p.basePosition = QPointF(0, 0))、src/BrushAnimationDialog.cpp:133 (durationSec)、呼び出し側 src/MainWindow.cpp:14900-14945
- 問題: `BrushAnimation::rebuildGeometry` は `QPainterPath::addText(m_basePosition, ...)` を使うので、basePosition はベースラインです。y=0 だと字形はほぼ全部が y<0 (キャンバスの上の外) に描かれます。また、ダイアログの「Duration」は params に入るだけで、MainWindow は `ensureBrushProgressTrack` で既定値 0 の `brush_progress` トラックを作るだけです (キーは打ちません)。描画側 (TimelineFrameRenderer.cpp:1960-1963、Overlay.cpp:369-371) は progress<=0 で何も描きません。
- 発生条件: 「ブラシアニメ」を追加して OK する。
- 影響: プレビューにも書き出しにも何も出ません。グラフエディタで brush_progress にキーを手で打っても、文字はフレームの上端の外にあり、ディセンダ (g, p, y) の一部しか見えません。
- 確度: 確認済み (params → setText(basePosition=(0,0)) → toJson → renderer の fromJson → renderFrame の経路を追跡)
- 修正方針: 基準位置をキャンバス中央 (ascent を考慮) などにし、追加時に brush_progress の 0→1 のキーを durationSec で打つ。

### [Medium] EXR シーケンスの取り込みが、EXR プラグインがあっても必ず失敗する
- 区画: part02
- 場所: src/BlenderExrReader.cpp:94-100 (exrSupported)、利用側 src/MainWindow.cpp:17999-18019
- 問題: `QImageReader::imageFormat(const QString &fileName)` はファイルを開いて中身から形式を判定する関数です。存在しない "test.exr" を渡しているので常に空を返し、`canReadExr` は常に false です。全フレームの image が null のままになります。
- 発生条件: 取り込みハブで EXR シーケンスを取り込む。
- 影響: MainWindow は先頭フレームを PNG に保存しようとして失敗し、「EXR取り込み失敗: 先頭フレームをPNG保存できません」で終わります。機能が一度も動きません。修正後も、全フレームをデコードして QImage として持ち、先頭しか使わないため、長いシーケンスでメモリを大量に使います。
- 確度: 確認済み (Qt の imageFormat(QString) は QFile::open に失敗すると空を返す)
- 修正方針: `QImageReader::supportedImageFormats().contains("exr")` で判定する。読み込みは必要なフレームだけにする (列挙だけして遅延読み込み)。

### [Medium] SCC 書き出しのドロップフレーム timecode が誤っており、字幕を消す EDM も出ない
- 区画: part02
- 場所: src/BroadcastCaption.cpp:129-158 (timecodeFromSec)、170-201 (toScc)
- 問題: (1) 29.97/59.94 で ';' (ドロップフレーム表記) を付けますが、値は「実フレーム数 / 30」というノンドロップの計算です。ドロップフレームの番号の飛ばしを行っていません。(2) cue の `endSec` を使わず、EDM (表示消去) を出しません。(3) `doc.channel` (CC1..CC4) を使わず、常に CC1 の制御コードを出します。(4) 608 の基本文字は ASCII と一部違います (0x2A='á'、0x5C='é'、0x5E-0x60、0x7B-0x7F)。それなのに '*' '\\' '^' '_' '`' '{' '|' '}' '~' をそのまま送ります。改行も空白に変わり、32 桁の折り返しもありません。
- 発生条件: 放送用 CC ダイアログで 29.97fps のまま SCC を書き出す。
- 影響: (1) 1 時間で約 3.6 秒ずつ、字幕が早く出ます (3600 秒は 00:59:56;12 になります。正しくは 01:00:00;00)。(2) 字幕は次の cue が来るまで画面に残り続け、最後の字幕は消えません。(3) CC2-4 を選んでも CC1 に入ります。(4) 記号が別の文字で表示されます。
- 確度: 確認済み (timecodeFromSec の計算を数値で確認。toScc に EDM の発行も channel の参照も無い)
- 修正方針: SMPTE のドロップフレーム変換 (10 分ごとの例外を除き、毎分 2 フレーム飛ばす) を実装する。endSec に EDM を出す。channel に応じて hi バイト (CC2 は 0x1C 系) を切り替える。特殊文字は 0x11 0x3x などの拡張コードに対応表で写すか、置換して警告する。

### [Medium] カメラモーションダイアログで OK すると、カメラ解析の写像・点群・レイヤー 3D 変換・ベジェハンドルが消える
- 区画: part02
- 場所: src/CameraMotionDialog.cpp:122-159 (camera())、src/MainWindow.cpp:15775
- 問題: `camera()` は新しい `Camera3D` を作り、基本状態・シェイク・表のキーフレーム (時刻 / 値 / 補間種別のみ) だけを入れ直します。`m_solveMapping`、`m_solvePoints`、`m_layerTransforms`、キーフレームの bezX1..bezY2 は引き継がれません。MainWindow はそれで `m_projectCamera` を置き換えます。(fov / 位置のクランプは README:670 に記載済みなので、ここでは対象外です。)
- 発生条件: 特徴点のカメラ解析を適用した後に、カメラモーションダイアログを開いて (何も変えずに) OK する。
- 影響: 点群の表示 (US-1817) と解析点へのヌル作成 (US-1818) の元データ、および時間範囲 (solveMapping) が消え、保存しても戻りません。ベジェ補間のキーは既定ハンドルに変わります。undo スロットもクリアされます (MainWindow.cpp:15776) ので、元に戻せません。
- 確度: 確認済み (camera() の構築内容と Camera3D のメンバーを照合)
- 修正方針: `Camera3D result = m_camera;` を起点にして、基本状態・シェイク・トラックだけを差し替える。ベジェハンドルは表の行に保持する。

### [Medium] バッチ書き出しで選んだプリセットの解像度が、現在のプロジェクトのタスクでは無視される。ユーザープリセットでは解像度もコーデックも無視される
- 区画: part02
- 場所: src/BatchExportDialog.cpp:194-211 (makeCurrentProjectTask)、src/BatchExportQueue.cpp:186-187, 38-55、src/MainWindow.cpp:18597-18598
- 問題: 現在のプロジェクトのタスクには `width/height = m_projectConfig` が入ります。`buildRenderJob` は `task.width > 0 ? task.width : defaults.width` なので、プリセットの "4K" / "720p" は解像度に効きません。また `presetDefaults()` は組み込みの 3 種しか知らず、ユーザープリセット名は 1080p / h264 / 20Mbps に落ちます。そこから取り込むのは autoLoudness だけです (BatchExportQueue.cpp:214-223)。
- 発生条件: 「現在のプロジェクトを追加」でプリセットに 4K / 720p / 保存済みユーザープリセットを選ぶ。
- 影響: 表にはプリセット名が「4K」と出るのに、プロジェクト解像度 (例: 1080p) で書き出されます。ユーザープリセットでは、そのプリセットの解像度・エンコーダー・ビットレートの代わりに 1080p の既定値が使われます。
- 確度: 確認済み (context.width の設定 → makeCurrentProjectTask → buildRenderJob を追跡)
- 修正方針: プリセットを選んだ場合はプリセットの解像度を優先する (またはダイアログで「プロジェクト解像度」を明示的に選ばせる)。ユーザープリセットの ExportConfig から width/height/codec/bitrate を取り込む。

### [Medium] 自動マッティングの「適用」結果がプレビューと違う (収縮・膨張・フェザーの半径が解像度に比例しない)
- 区画: part02
- 場所: src/AutoMatteDialog.cpp:219-257 (rebuild)、22 (kProcessMax=256)
- 問題: プレビューは 256px に縮小した画像で、適用は元解像度で、どちらも同じ「px」半径の erode/dilate/feather を掛けます。1920 幅の素材では、プレビューの 2px は元解像度の約 15px に相当します。それなのに適用時は 2px のままです。また、適用時の erode/dilate はメインスレッドで O(w·h·(2r+1)²) 回の計算をします。
- 発生条件: 1080p 以上のフレームでフェザー / 収縮 / 膨張を設定して「適用」する。
- 影響: 書き出した PNG の境界は、プレビューよりはるかに硬く、ノイズ除去もほとんど効きません。4K で半径 32 を指定すると、約 350 億回の比較になり、UI が長時間固まります。
- 確度: 確認済み (rebuild(true) は m_source をそのまま使い、m_params を縮尺せずに渡す)
- 修正方針: 半径を「プレビュー基準」として、適用時に元幅 / プレビュー幅で拡大する。erode/dilate は分離可能な van Herk 法などにし、ワーカースレッドで実行する。

### [Medium] 字幕エディタの時刻スピンが 1 時間で頭打ちになり、1 時間を超える字幕の時刻を壊す
- 区画: part02
- 場所: src/CaptionEditorDialog.cpp:117, 171, 176 (setRange(0, 3600000))、527-529、559-577
- 問題: 開始 / 終了 / 分割のスピンの上限は 3,600,000 ms です。行を選ぶと `setValue` がクランプされた値を表示します。その後どちらか一方のスピンを編集すると、`onClipTimeEdited` が両方のスピンを読み、もう一方のクランプ値でクリップを上書きします。
- 発生条件: 1 時間を超える素材 (講義・配信など) の SRT を取り込むか音声認識し、1:00:00 以降の字幕の開始か終了を調整する。
- 影響: 編集していない側の時刻が 01:00:00.000 に書き換わります (開始 > 終了になり、適用時にエラーになる場合もあります)。1 時間以降の位置では分割もできません。
- 確度: 確認済み (QSpinBox のクランプ → onClipTimeEdited が両方の値を代入)
- 修正方針: 上限を INT_MAX 程度 (または素材の長さ) にする。変更されたスピンの値だけを反映する。

### [Medium] エフェクトコントロールが Timeline のクリップを const_cast で直接書き換え、undo 履歴のスナップショットまで変わる
- 区画: part03
- 場所: src/EffectControlsPanel.cpp:292（selectedClipForKey の const_cast）, 435, 1188, 1213 / src/EffectKeyframeNavBar.cpp:71-74, 255, 270, 385, 434-438, 447, 473 / src/EffectControlsPanel.cpp:744, 782
- 問題: `TimelineTrack::clips()` は `const QVector<ClipInfo>&`（Timeline.h:553）で、`saveUndoState` は `currentState()` で同じ QVector を暗黙共有のままコピーして undo スタックに積む（Timeline.cpp:16645, UndoManager.cpp:11）。パネルとナビバーは const_cast した ClipInfo / KeyframeTrack を直接書き換えるため、detach が起きず、共有中のバッファ＝直近（そのトラックが最後に変わって以降の全）スナップショットも同時に書き換わる。
- 発生条件: (a) プリセット適用（435 で `clip->colorCorrection` を直接代入した後に setClipEffectsAndKeyframes）、(b) キーフレームの ON/OFF（1188/1213）、(c) ナビバーでキーフレームのドラッグ・補間変更・削除・追加 → いずれも Ctrl+Z。
- 影響: undo で戻した先の状態にも変更が入っているため、プリセットの色補正やキーフレームの追加/削除が undo で戻らない（エフェクト列だけ戻り、色補正は残る等の不整合）。V2 以上では前項により undo 点も作られないので、変更が無記録で残る。ナビバーはクリップ内部への生ポインタを保持するので、パネル更新前に clips が置き換わるとダングリングの恐れもある。
- 確度: 確認済み（暗黙共有と const_cast による書き込み経路を追跡。ダングリングは可能性あり）
- 修正方針: ClipInfo のコピーを編集して Timeline のトラック指定 setter に渡す形に統一し、const_cast と生ポインタ保持をやめる（ナビバーは編集後のトラックを signal で返す）。

### [Medium] エフェクトコントロールの各パラメータ欄がクリップの実際の値ではなくスキーマ既定値を表示し、キーフレーム作成も既定値で行う
- 区画: part03
- 場所: src/EffectRowWidget.cpp:193-199, 288-299, 325-326, 362-367 / src/EffectControlsPanel.cpp:1274-1279
- 問題: `setEffect` は `m_effect` を保存するだけで、`buildRows` は spin/slider/checkbox/combo を `def.defaultVal` で初期化する。`paramValue(m_effect, def.name)` を一切参照しない（色だけ例外）。キーフレーム ON 時の初期値は `getParamValueByName` = spin の値 = 既定値。
- 発生条件: 既定値以外のパラメータ（例: Blur radius=20）を持つクリップを選択してエフェクトコントロールを見る / キーフレームボタンを押す / spin を 1 段動かす。
- 影響: 表示値が実際の値と違う。キーフレームを打つと値が既定値（radius=5）へ飛ぶ。spin を 1 段動かすと既定値±1 段で実値を上書きする。
- 確度: 確認済み
- 修正方針: buildRows で `effectctrl::paramValue(m_effect, def.name)` / Bool・Choice も同様に現値で初期化する（signal 接続前に設定）。

### [Medium] プリセット（キーフレームを含まないもの）を適用すると、旧エフェクトの `effect.N.*` キーフレームが新しいエフェクトに掛かる
- 区画: part03
- 場所: src/EffectPreset.cpp:360-366 / src/EffectControlsPanel.cpp:432-440 / src/EffectLibraryModel.cpp:1047-1049
- 問題: `applyToClipStack` は `clip.effects` を丸ごと差し替えるが、`replaceEffectKeyframes` は `includesKeyframes` のときだけ呼ばれる。エフェクトのキーフレームは index 基準（`effect.%1.%2`、ClipAnim.cpp:432）なので、旧スタックのトラックが新スタックの同じ index・同名パラメータへ適用される。
- 発生条件: Blur の radius にキーフレームがあるクリップへ組み込みプリセット「Cinematic Warm」（index0 が Vignette、radius あり）を適用する。
- 影響: Vignette の radius が旧 Blur の値（0..1 にクランプ）で動き、見た目が壊れる。別名なら孤立トラックとして残り、後でエフェクトを足すと突然効き始める。
- 確度: 確認済み
- 修正方針: エフェクト列を差し替えるときは常に `effect.*` トラックを消す（includesKeyframes なら置換、なければ削除のみ）。

### [Medium] Windows で「配信認証情報」ダイアログを開くだけで YouTube / Vimeo のトークンと Twitch の stream key が消える
- 区画: part03
- 統合メモ: 同根: part14(Twitch のストリームキー平文保存)、part15(Vimeo token が vault 非対応)、part16(YouTube refresh_token が base64 平文)。直すなら配信系 3 クライアントを CredentialStore 経由に統一する。
- 場所: src/CredentialDialog.cpp:307 / src/CredentialStore.cpp:146-167, 13-29 / 読み手: src/YoutubeOAuth.cpp:370-386, src/VimeoOAuth.cpp:81-83, 311-315, src/TwitchStreamDialog.cpp:100, 127
- 問題: ダイアログの ctor が `migrateSensitiveKeysToVault()` を呼び、sensitiveKeyList の値（youtube_oauth/refresh_token、vimeo_oauth/access_token・refresh_token、twitch/stream_key）を Credential Manager へ移して QSettings から削除する。しかし YouTube / Vimeo / Twitch の各モジュールは CredentialStore を通さず QSettings を直接読み書きしている。
- 発生条件: Windows（Vault 対応時）で一度ログイン/保存した後、配信認証情報ダイアログを開く。
- 影響: 次回起動以降、YouTube/Vimeo は refresh_token が無く再認証が必要になり、Twitch ダイアログの stream key が空になる。再保存すると平文の QSettings に戻り、次にダイアログを開くとまた消える（ループ）。YouTube の refresh_token は base64 のまま Vault に入り、誰も復号しない。
- 確度: 確認済み（Windows のみ。非 Windows は isSupported()=false で no-op）
- 修正方針: 各モジュールの読み書きを `CredentialStore::get/set` に統一するまで migrate を止める。YouTube の base64 表現も CredentialStore 経由で扱う。

### [Medium] DaVinci Resolve XML 書き出しが、タイムライン上の位置をソースの in/out として書き、start/end を出力しない
- 区画: part03
- 場所: src/DavinciResolveXmlExporter.cpp:85-99 / src/MainWindow.cpp:2063-2068
- 問題: `buildDavinciExportClips` は `inPoint`/`outPoint` にタイムライン位置（cursorSec*fps）、`sourceIn` にソースの in を入れるが、`buildXml` は `<in>`/`<out>` に inPoint/outPoint（＝タイムライン位置）を書き、`sourceIn` を使わない。xmeml の配置用 `<start>`/`<end>` も出ない。速度も反映されない。
- 発生条件: 2 本目以降のクリップ（タイムライン 0 秒以外）や、in 点が 0 でないクリップを含むタイムラインを DaVinci XML で書き出す。
- 影響: Resolve で読むと素材の使用区間がタイムライン位置分ずれ（例: 10 秒位置のクリップは素材の 10 秒目から）、配置位置も失われる。
- 確度: 確認済み
- 修正方針: `<start>/<end>` に inPoint/outPoint、`<in>/<out>` に sourceIn / sourceIn+duration*speed を書く。

### [Medium] エフェクトライブラリの「Color Shift」プラグインを適用するとクリップが真っ黒になる（サムネイルは無変化）
- 区画: part03
- 場所: src/EffectLibraryModel.cpp:214, 286-290, 834-839, 1051-1055 / src/EffectPlugin.h:109-111 / src/VideoEffect.cpp:3326-3328
- 問題: プラグイン項目を clip に適用する時は `buildVideoEffect` で ChannelMixer に写像し、プラグインの既定値（Red/Green/Blue Shift = 0、範囲 -100..100 の加算量）をそのまま `redFromRed` 等（0..200 % の乗数、既定 100）へ入れる。Edge Detect の Threshold 既定 30（0..255）も FindEdges の intensity（0..2）へそのまま入る。一方サムネイル/applyToImage はプラグインの `process()` を直接使う。
- 発生条件: エフェクトライブラリで「Color Shift」（または Edge Detect）をダブルクリック／適用。
- 影響: Color Shift は gain 0 で全チャンネル 0 → 黒い映像。Edge Detect は強度 30 倍で白飛び。サムネイル（無変化）と適用結果が一致しない。
- 確度: 確認済み
- 修正方針: 単位の違うパラメータは写像しない（または変換式を入れる）。対応が取れないプラグインは clip 適用不可にする。

### [Medium] 「HDR 出力設定」ダイアログのモード(HLG)と MaxCLL / マスタリング輝度が書き出しに届かない
- 区画: part04
- 場所: src/HDRSettingsDialog.cpp:81-91、src/MainWindow.cpp:16096-16101(`m_hdrSettings` に入れるだけ)、src/ExportDialog.cpp:89-92(`toJobJson`)、src/RenderQueue.cpp:1571-1574 / :1734-1739 / :2210-2216
- 問題:
  - `m_hdrSettings` は保存されますが、使われるのは Dolby Vision のサイドカーだけです(MainWindow.cpp:11357-11360)。ExportDialog の `ExportConfig::hdrSettings` は既定値(sdr)のままです。
  - RenderQueue は `hdrMasterMaxLum` / `hdrMasterMinLum` / `hdrMaxCll` / `hdrMaxFall` を読みますが、これらのキーを job に書く場所がどこにもありません(grep で確かめました)。そのため、常に 1000 / 0.0001 / 1000 / 400 で書き出されます。
  - HLG を選ぶ経路がありません。仮に hdrSettings.mode が "hlg" になっても(ユーザープリセットの JSON など)、`toJobJson` は `hdr10 = true` と `hdrMode = "hlg"` を両方書きます。RenderQueue はこれを `isHdr10 = true` かつ `isHlg = true` と解釈し、Encode.cpp:463 / :559 では HDR10 が優先されるため、PQ のタグで書き出されます。
- 発生条件: 「HDR 出力設定」で HLG を選ぶか、MaxCLL / MaxFALL / 輝度を変えてから、HDR10 のプリセットで書き出す。
- 影響: 設定した HDR のメタデータやモードが黙って無視され、既定値の HDR10 になります。
- 確度: 確認済み(キーを読む側と書く側の両方を grep で確かめました)
- 修正方針: `buildExportRenderJob` / `toJobJson` で `m_hdrSettings` の mode と MaxCLL などを `hdrMode` / `hdrMaxCll` などのキーに書く。HLG のときは `hdr10` を立てない。

### [Medium] FractalNoise の FBm が [-1,1] の値を [0,1] に切り詰め、画面のほぼ半分が黒になる(DisplacementMap の手続き的マップも片寄る)
- 区画: part04
- 場所: src/FractalNoise.cpp:220-223(「Normalise to [0, 1]」とあるが FBm は ±1 のまま)、:253-255 / :272-273(0 未満を 0 に切り詰める)、利用側 src/VideoEffect.cpp:2298-2306 / :2321-2323 / :2343-2353、src/NodeLibrary.cpp:509-518
- 問題:
  - Perlin ノイズは 0 を中心とする対称な値です。FBm は `value / maxVal` で [-1,1] になるだけで、0.5 を足していません。
  - `render` は 0 未満を 0 に切るため、約半分の画素が黒(0)になります。
  - DisplacementMap(mode 0)はマップを `m*2-1` で [-1,1] に戻すため、画素のほぼ半分が一律 `-hAmt` / `-vAmt` だけずれ、ずれの向きも負側に片寄ります。
- 発生条件: FractalNoiseGen エフェクト、ノードの FractalNoise(kind=FBm、既定)、DisplacementMap の手続き的マップ(param3=0)を使う。
- 影響: ノイズの絵が半分つぶれ、ディスプレイスメントは対称に揺れず、片側に寄ります。プレビューと書き出しの両方に出ます。
- 確度: 確認済み(式と利用側の再マップを確かめました。FBm の平均は 0 付近で、0 未満の画素が約半分になります)
- 修正方針: FBm は `value = value * 0.5 + 0.5`(Turbulence / Ridged は現状どおり)で [0,1] に写す。sample() のドキュメントと合わせる。

### [Medium] 3D テキストの押し出しで、文字の穴(O / A / 8 / あ など)が塗りつぶされる
- 区画: part04
- 場所: src/ExtrudedMesh.cpp:97-122(`triangulateRing` の重心ファン)、:148-179 / :289-321(輪郭ごとに独立して前後のキャップを張る)、利用側 src/Text3DLayer.cpp:589-597
- 問題:
  - `glyphContours` は外周と内側の穴を別々の輪郭として返します。`buildExtrudedMesh` は穴も含め、各輪郭を自分の重心からファン分割して前面と背面のキャップを張ります。
  - そのため、穴の輪郭にも塗られた面ができ、外周のキャップと重なって穴がふさがります。
  - ベベルの縮小も、穴に対しては穴の重心へ向かうので、方向が逆になります。
  - ヘッダに書かれた制限は「凹形の輪郭」だけで、穴については書かれていません。
- 発生条件: 押し出しを有効にした 3D テキストに、穴のある文字(O, A, B, D, P, R, 0, 4, 6, 8, 9, あ, お, ゆ など)を含める。
- 影響: 文字が塗りつぶされた塊になり、プレビューと書き出し(Wave 17 以降は書き出しにも乗る)の両方で文字として読めません。
- 確度: 確認済み(三角形分割と、キャップを輪郭ごとに張るコードを読みました)
- 修正方針: QPainterPath::toFillPolygons、または穴を外周につなげる bridge 付きの ear clipping(あるいは Qt の三角形分割)でキャップを作る。少なくとも、外周に含まれる穴の輪郭にはキャップを張らない。

### [Medium] GPU プレビューのエフェクトが、スタックの順序と一部のパラメータを書き出しと違う形で適用する
- 区画: part04
- 場所: src/GLPreview.cpp:2383-2425(m_videoEffects をフラグに畳む)、シェーダ :1103-1108(Sepia → Gray → Invert → Vignette → Noise の固定順)、:739-756(fxBlur の半径を 8 に制限、1 回のボックス)、比較先 src/VideoEffect.cpp:1966-1972(`applyEffectStack` はリストの順)、:2231-2243(Grayscale は param1 を無視)、:1976-(Blur は 3 パスのボックス)、ルーティング src/VideoPlayer.cpp:92-100
- 問題:
  - GPU 対応 6 種(Blur / Noise / Sepia / Grayscale / Invert / Vignette)だけのスタックは全部 GPU に回りますが、GLPreview はリストの順序を捨て、シェーダの固定順で適用します。同じ種類が 2 つあっても 1 つに潰れます。
  - Grayscale / Invert は GPU だけが param1 を強度として使います(CPU は常に 100%)。Blur は GPU が半径 8 に切り詰めた 1 回のボックス、CPU は半径 r の 3 パスのボックスです。
- 発生条件: 例: [Grayscale, Sepia] のスタック(書き出しはセピア調、プレビューはグレー)。Grayscale の param1 が 0 と 1 の間。Blur の param1 が 8 より大きい。
- 影響: プレビューと書き出しで色やぼけの量が違います。
- 確度: 確認済み(フラグへの畳み込みとシェーダの順序、CPU の適用順を確かめました)
- 修正方針: スタックが GPU の固定順と一致しないとき、同じ種類が重複するとき、パラメータの意味が違うときは `stackRequiresClipLocalCpu` を true にして CPU 経路に回す。または GPU の式を CPU に合わせる。

### [Medium] FCPXML 読み込みで、multicam の入れ子が分岐すると角度の文書を指数回パースする(細工したファイルで固まる)
- 区画: part04
- 場所: src/FcpxmlImport.cpp:410-417(`appendCut` が呼ばれるたびに `parseDocument(angle.document)`)、:365-373(循環と深さ 8 だけを検査)、:1187-1196(`parseXml` は depths が収束するまで全体を繰り返す)
- 問題:
  - compound(media/sequence)は `nest.built` でキャッシュされます。multicam の角度の文書には同じキャッシュがなく、mc-clip が現れるたびに、その角度をパースし直します。
  - 角度の中の mc-clip が別の multicam を参照すると再帰します。循環(chain)と深さ(8)は止めますが、DAG の分岐は止めません。各段に k 個の mc-clip があると、パースは k^8 回になります。結果のエントリ数も同じ割合で増えます。
- 発生条件: multicam M1 の角度に M2 を参照する mc-clip を 20 個置き、M2 の角度に M3 を参照する mc-clip を 20 個置く、という構造を 8 段重ねた FCPXML を読み込む(GUI、または MCP の `import_fcpxml`)。
- 影響: 20^8(約 2.6×10^10)回のパースが走り、UI スレッドが事実上永久に固まります。メモリも使い果たします。
- 確度: 確認済み(静的に再帰の経路を追いました。実際には動かしていません)
- 修正方針: 角度の文書のパース結果(エントリ)を ref ごとにキャッシュして切り出しだけを繰り返す。展開した総エントリ数や総パース回数に上限を設ける。

### [Medium] FCPXML 書き出しの asset の duration が最初に使ったクリップの尺で、start=0s 固定のため、後のクリップの範囲が asset の外に出る
- 区画: part04
- 場所: src/FcpxmlExporter.cpp:136-151(asset は filePath ごとに最初の ClipEntry の `duration` を使う)、:228-240(asset-clip の start は `startInSource`)、:315-352(`buildExportClips` は duration にタイムライン上の尺を入れ、speed を書かない)、:242-278(spine は V1 クリップと子を持つ gap だけで、V1 の隙間を gap で埋めない)
- 問題:
  - asset の `duration` には素材全体の長さではなく、最初のクリップのトリム後の尺(effectiveDuration)が入ります。
  - 同じ素材を in 点 10s で使うクリップ(start=10s)や、2 本目以降で別の範囲を使うクリップは、asset の範囲 [0, duration) の外を参照します。
  - speed ≠ 1 のクリップは、タイムライン上の尺をそのまま素材の尺として書くため、ソースの範囲がずれます。
  - V1 の leadInSec(隙間)に接続クリップが無いと gap 要素を出さないため、spine の中で隙間が表現されません。
- 発生条件: トリムした素材、同じ素材を複数回使うタイムライン、速度変更、V1 に隙間のあるタイムラインを FCPXML で書き出す。
- 影響: Final Cut Pro などで、範囲外のクリップがオフラインやエラーになる可能性、速度と位置がずれる可能性があります。自前の読み込みは `c.duration = max(outPoint, asset.duration)` で吸収するため、往復のテストでは見つかりません。
- 確度: 可能性あり(書き出す値は確認済みです。Final Cut Pro の実機で、エラーになるか詰められるかは確かめていません。README も実機確認は未実施としています)
- 修正方針: asset の duration には素材の実際の長さ(ClipInfo::duration)を書く。速度は timeMap で書く。V1 の隙間には gap を出す。

### [Medium] LUT_3D_SIZE 1 / LUT_1D_SIZE 1 の .cube で範囲外読み出し (クラッシュ)
- 区画: part05
- 場所: src/LutImporter.cpp:86-97 (検証), src/LutImporter.cpp:106-137 (trilinearInterpolate), src/LutImporter.cpp:181-191 (1D)
- 問題: 表の長さ検証は size^3 (1D は size) と一致するかだけで、size>=2 を要求しない。size=1 だと `qMin(int(rIdx), size-2)` が -1 になり、`idx(-1,-1,-1) = -3` などの負の添字で `table[]` (QVector::operator[]、範囲検査なし) を読む。1D は `qBound(0, v, size-2=-1)` が 0 を返し、`table[1]` (要素数 1) を読む。さらに `lut.size * lut.size * lut.size` は int で計算するため size>=1291 で桁あふれし、あふれた値と同じ行数の表を持つファイルも検証を通って巨大な添字で読む。
- 発生条件: `LUT_3D_SIZE 1` と 1 行の RGB だけの .cube を、LUT 適用 / LUT 管理 / プロジェクトの clip.lutFilePath (MCP の apply_lut を含む) で指定する。書き出し時は TimelineFrameRenderer.cpp:736-737 の gradeClipNativeFrame がフレームごとに loadCubeFile → applyLutWithIntensity を呼ぶ。
- 影響: 不正な (または手で作った) LUT ファイル 1 つでプレビュー生成 / 書き出しがヒープ外を読んでクラッシュする。プロジェクトを開いて書き出すだけで起きる。
- 確度: 確認済み (loadCubeFile の検証 → isValid() → applyLut → trilinearInterpolate の添字計算を追った)
- 修正方針: 3D は 2 <= size <= 256 程度、1D は size >= 2 を必須にし、要素数の計算は qint64 で行う。

### [Medium] 縁取り (stroke) の計算が幅の 2 乗回の全面描画になり、GUI の上限 200px でも 1 フレームに数分かかる
- 区画: part05
- 場所: src/LayerStyle.cpp:105-144 (outsideStrokeAlpha), src/LayerStyle.cpp:57-103 (boxBlurAlpha)
- 問題: outsideStrokeAlpha は半径 r の円内の (dx,dy) ごとにレイヤー全体を `p.drawImage(dx, dy, alpha)` する。r=200 だと約 π·200² ≈ 12.6 万回、キャンバスと同じ大きさの画像を描く。boxBlurAlpha も素朴な畳み込みで画素ごとに (2r+1) 回 × 2 パス。EffectControlsPanel.cpp:616/639 は幅とぼかし半径に 0〜200 を許しており、ProjectFile.cpp:2212 の LayerStyle::fromJson には上限がない (巨大な値では `static_cast<int>(std::ceil(...))` が UB)。
- 発生条件: クリップのレイヤースタイルで縁取り幅を数十〜200 にする。TimelineFrameRenderer.cpp:2979 ほか、および VideoPlayer.cpp:810 ほかがフレームごとに layerstyle::apply を呼ぶ。
- 影響: プレビューと書き出しが事実上止まる (1080p で 1 フレームに数分)。手で編集した、あるいは外から受け取ったプロジェクトでは大きな値で無限ループ相当や UB になる。
- 確度: 確認済み (ループ回数をコードから数えた。実測はしていない)
- 修正方針: 縁取りは距離変換か、分離可能な max フィルタによる膨張にする。ぼかしは累積和の box blur にする。fromJson で値を GUI と同じ範囲に丸める。

### [Medium] ラウドネスパネルの測定結果が無音 (-70 LUFS) でも「正規化」で +56 dB を書き出しに設定する
- 区画: part05
- 場所: src/LoudnessPanel.cpp:170-181, MainWindow.cpp:22037-22052 (applyLoudnessNormalize。範囲外の参照先)
- 問題: onNormalizeClicked は `m_hasMeasurement` だけを見て `gain = target - m_integratedLUFS` を emit する。LoudnessAnalyzer は無音 / ゲート以下で -70 (kNoSignalLufs) を返すため、YouTube 目標では +56 dB になり、Exporter と RenderQueue の setLoudnessGainDb にそのまま入る (RenderQueue::buildLoudnessAudioFilterArgs が `volume=56dB,alimiter`)。自動ラウドネスの経路 (RenderQueue::measureAutoLoudness) には `measured <= -70` で飛ばす処理があるが、パネルの経路にはない。
- 発生条件: 音声がほぼ無音のタイムライン (ルームトーンだけ、-70 LUFS 以下) で「測定」→「ターゲットに正規化」→ 書き出し。
- 影響: 書き出した音声でノイズフロアが 56 dB 持ち上がり、リミッターで潰れた爆音のノイズになる。
- 確度: 確認済み
- 修正方針: integrated <= -70 (または有限でない値) のときは正規化を拒否してヒントを出す。ゲインにも上限 (例 +20 dB) を設ける。

### [Medium] ラウドネスパネルの測定が、トラックのフェーダー / パン / ソロ / バス / FX と速度変更を反映せず、書き出しと値が食い違う
- 区画: part05
- 場所: src/MainWindow.cpp:2216-2367 (decodeEntryForLoudness / measureTimelineLoudness)
- 問題: PlaybackEntry にはクリップの volume / envelope / トランジションしかなく (PlaybackTypes.h)、測定は AudioMixer の trackGain / trackPan / バスの経路 / ソロ / トラックとマスターの FX をまったく使わない (書き出しは AudioExportMix.cpp:236-269 でこれらをすべて適用する)。速度変更したクリップでは、ソースのサンプルを時間伸縮せずに `llround((timelineStart + (src-clipIn)/speed)*48000)` の位置へ足し込む。speed=2 では 2 サンプルが同じ位置に加算され (低域で約 +6 dB)、speed=0.5 では 1 つおきの位置が 0 になる (約 -3 dB)。
- 発生条件: A1 のフェーダーを -6 dB にする、別トラックをソロにする、速度 2x のクリップを置く、などの状態でパネルから測定して正規化する。
- 影響: 測定値が書き出しの実際の音量からずれ、その差がそのまま正規化ゲインの誤差になる (例: フェーダー -6 dB なら書き出しは目標より 6 dB 小さい)。
- 確度: 確認済み (測定経路に mixer の参照がないこと、速度の写像を確認した)
- 修正方針: 測定には書き出しと同じ audioexport::buildMixPlan / executeMixPlan で作ったミックスを使う (RenderQueue の自動ラウドネスと同じ方式)。

### [Medium] ラウドネス計測がステレオを BS.1770 どおりに扱わず、通常のステレオ素材を最大約 3 dB 小さく測る (結果として正規化後の書き出しが目標より大きくなる)
- 区画: part05
- 場所: src/LoudnessAnalyzer.cpp:190-192 (チャンネル平均), src/LoudnessMaster.cpp:244-283 / 414-455 (swr でモノにダウンミックスしてから測定)
- 問題: BS.1770 / R128 は各チャンネルの K 特性エネルギーの「和」(L/R は重み 1.0) を取る。LoudnessAnalyzer は `frameEnergy /= channels` で平均にしており、無相関のステレオでは -3.01 dB になる。measureIntegratedLufs (RenderQueue::measureAutoLoudness が書き出しの自動ラウドネスに使う) は swr で mono (FL/FR → FC を 0.707 ずつ) にしてから 1 チャンネルとして測るため、L=R なら一致するが、無相関なら -3 dB、逆相成分はさらに小さく測る。5.1 では LFE を除外することもサラウンドの重み 1.41 を掛けることもしていない。
- 発生条件: ステレオ幅のある BGM や音楽で、パネル測定または自動ラウドネス付きの書き出しを行う。
- 影響: YouTube / Spotify などの BS.1770 準拠の計測と比べて最大 3 dB 小さく出る。正規化ゲインがその分大きくなり、-14 LUFS 目標の書き出しが実際には -11 LUFS 前後になる (配信側で下げられるか、alimiter で潰れる)。
- 確度: 確認済み (式の違い)。ずれの量は素材の相関に依存する (swr の既定行列の係数は FFmpeg の rematrix に基づく推定)。
- 修正方針: チャンネル別に K 特性フィルタを通し、重み付きの和でブロックエネルギーを作る (L/R/C=1.0、Ls/Rs=1.41、LFE は除外)。measureIntegratedLufs もダウンミックスせずにチャンネル別に計測する。

### [Medium] ノードモードを ON→OFF するだけで、クリップのエフェクトの無効化フラグとエフェクト別マスクが消え、選択が変わっていれば別クリップに書き込まれる
- 区画: part05
- 統合メモ: part06 の同旨指摘(ノード OFF 時に別クリップへ書き込む)を統合。
- 場所: src/LayerNodeBridge.cpp:76-89, 167-211, 217-266 (+ 呼び出し元 MainWindow.cpp:22125 / 22134-22140、Timeline.cpp:11891-11899)
- 問題: fromEffectStack は type と param1 (Blur の radius) だけをノードに写し、toEffectStack は `VideoEffect::createBlur(radius)` / `createInvert()` で作り直す。そのため `enabled=false`、`effect.mask` (effectmaskui::updateMask で設定するエフェクト別マスク)、ほかのフィールドは既定値に戻る。OFF にすると MainWindow は isLinearChain と toEffectStack が成功するたびに `m_timeline->setClipEffects(effects)` を呼ぶが、これは開いたクリップ (m_nodeModeClipId) ではなく、その時点のアクティブトラックで選択されているクリップへ書き込む。
- 発生条件: Blur / Invert だけ (またはエフェクトなし) のクリップでノードモードを ON にし、何も編集せず OFF にする。ノードモード中に別のクリップを選択してから OFF にした場合は、そのクリップの effects が置き換わる (エフェクトなしの graph なら空にされる)。
- 影響: 無効にしておいたエフェクトが勝手に有効になり、エフェクトのマスクが消える。別クリップのエフェクトスタックが黙って上書きされる (undo は 1 段で戻せる)。
- 確度: 確認済み
- 修正方針: 往復変換では元の VideoEffect を node の params に丸ごと保持して書き戻す。書き戻し先は m_nodeModeClipId (トラックとクリップの位置) を明示した setClipEffectsAndKeyframes にする。graph が未編集なら書き戻さない。

### [Medium] 3D ライトダイアログで、キーの間の時刻に値を 1 つ変えると、ほかのアニメーション済みプロパティにも「基準状態」の値でキーが打たれる
- 区画: part05
- 場所: src/Light3DDialog.cpp:311-345 (refreshInspector), 389-415 (stateFromWidgets / syncCurrentLightFromWidgets), src/Light3D.cpp:360-372 (setStateAt)
- 問題: インスペクタは `m_lights.at(row).state()` (アニメーションしない基準状態。最後に setKeyframe した値) を表示し、キーフレーム時刻スピンを変えても stateAt(time) で表示を更新しない。spin を 1 つ変えると stateFromWidgets が全ウィジェットの値を集め、setStateAt がキーを持つ全プロパティに時刻 T でキーを追加する。setKeyframe は 14 プロパティすべてにキーを打つので、変えていない位置 / 色 / 強度なども、補間値ではなく基準状態の値で T に上書きされる。
- 発生条件: t=0 と t=5 に「現在値を追加」でキーを打つ → 時刻スピンを 2.5 にする → 強度だけを変える。
- 影響: 位置などが t=2.5 で t=5 の値へ跳び、アニメーションが壊れる。見た目では原因が分かりにくい。
- 確度: 確認済み
- 修正方針: 表示は stateAt(keyframeTime) から作り、時刻スピンの変更でも refreshInspector を呼ぶ。setStateAt は変更されたプロパティだけにキーを追加する。

### [Medium] インクリメンタル保存が saveProjectToPath を通らず、一時ディレクトリの ImportHub 素材を移さないまま保存する
- 区画: part05
- 場所: src/MainWindow.cpp:4122-4145
- 問題: 通常の保存 / 名前を付けて保存は saveProjectToPath (MainWindow.cpp:10366-) で、OS の一時ディレクトリ (/tmp/v-simple-editor-importhub) にある ImportHub 生成 PNG を `<name>_assets/importhub` へコピーし、パスを張り替えてから保存する (US-1506)。インクリメンタル保存は populateProjectData → ProjectFile::save を直接呼ぶため、この移動が起きない。setWindowModified(false) も呼ばない。
- 発生条件: 未保存の状態で ImportHub (PSD / メッシュのプレビューなど) から素材を取り込み、一度も通常保存せずに Ctrl+Alt+Shift+S で保存する。または通常保存したあとに取り込み、インクリメンタル保存だけを使う。
- 影響: 保存した版 (_v002 など) が一時ディレクトリのファイルを参照したままになり、再起動や一時ファイルの掃除でメディアがオフラインになる。タイトルの変更マーク (*) も消えない。
- 確度: 確認済み
- 修正方針: インクリメンタル保存も `saveProjectToPath(targetPath, &error)` を呼ぶ。

### [Medium] 「新規プロジェクト」がタイムラインも保存先パスも初期化せず、undo 履歴だけを消す
- 区画: part06
- 場所: src/MainWindow.cpp:10183-10218
- 問題: newProject は設定・カメラ・ライト・overlay・パーティクルなどを初期化するが、タイムラインのクリップ、`m_projectFilePath`、roto / タイムリマップ / マット / 親子、メディアプール、バスルーティング、ACES、Dolby Vision、字幕、マーカー、ノードグラフを初期化しない。それなのに `undoManager()->clear()` で履歴を消す。
- 発生条件: プロジェクト A を開いた (または保存した) 状態で「新規」→ 設定ダイアログで OK。
- 影響: 前のクリップがそのまま残った「新規」プロジェクトになり、タイトルも A のファイル名のまま。ここで Ctrl+S すると確認なしで A.veditor が新しい名前・解像度で上書きされる。undo 履歴が消えているので元にも戻せない。(未保存確認が無い件は AUDIT 既出だが、こちらは初期化そのものの欠落)
- 確度: 確認済み (newProject → applyProjectConfig → Timeline::setProjectOutputConfig を追跡。どこにもクリア処理が無い)
- 修正方針: 空の ProjectData を applyLoadedProjectData(data, QString()) に通す形にして、全状態と m_projectFilePath を一括で初期化する。

### [Medium] シーンカット検出 / シーン検出のマーカーと分割位置が、クリップの in 点・速度・タイムライン位置を無視する
- 区画: part06
- 統合メモ: part09 の同旨指摘(SceneCutScanner 側の時刻基準)を統合。
- 場所: src/MainWindow.cpp:15893-15921 (onSceneCutDetect)、:21185-21207 (openSceneDetector)
- 問題: SceneCutDialog は素材ファイル全体を走査し、素材先頭からの時刻を返す (SceneCutDialog.cpp:150, :206)。onSceneCutDetect はそれを `clipStartSec + ms/1000` でタイムライン時刻にしており、`inPoint`・`speed`・逆再生・タイムリマップを考慮しない。素材の使用範囲外のカットも捨てない。openSceneDetector はクリップの開始位置すら足さず、素材時刻をそのままタイムライン時刻としてマーカーを置く。
- 発生条件: in 点 > 0、速度 ≠ 1、または V1 の 2 本目以降のクリップでシーン検出する。
- 影響: マーカーがずれた位置に付く。「分割」モードでは再生ヘッドを素材時刻相当の位置へ動かして splitAtPlayhead するため、選択クリップ外 (後続の別クリップ) を分割してしまう。分割は 1 回ごとに undo 点になり、まとめて戻せない。
- 確度: 確認済み (時刻変換式と splitAtPlayhead の対象を確認)
- 修正方針: 素材時刻を [inPoint, outPoint] でフィルタし、clip.timelineSecondForSource 相当 (速度・逆再生・リマップ込み) でタイムライン時刻へ写す。分割は 1 つの undo 点にまとめる。

### [Medium] いくつかの編集操作に undo 点が無い、または 3D 設定を消す (属性ペースト / エフェクトペースト / 自動カット / マルチカム適用)
- 区画: part06
- 場所: src/MainWindow.cpp:11583-11602 (pasteAttributes)、:11538-11553 (pasteEffects)、:17671-17680 (openAutoClipDialog)、:21132-21134 (onMultiCamApplyToTimeline)
- 問題:
  - `Timeline::setClipMotion` は saveUndoState しない (Timeline.cpp:9465-9504。MCP 側は直後に保存している)。pasteAttributes で位置/スケール/回転/不透明度だけを貼ると undo 点が作られない。
  - pasteEffects は `is3DLayer=false`、posZ / worldX/Y / rotX/Y/Z=0 の MotionState を渡すため、貼り付け先の 3D レイヤー設定が全部消える。さらにモーションは effects の undo 点の後に変わるので redo で再現されない。
  - 自動カットは `track->addClip` を直接呼び、マルチカム適用は V1/A1 を setClips で丸ごと置き換えるが、どちらも saveUndoState もロック確認もしない。位置キーの sidecar (roto / マットなど) の付け替えも無い。
- 発生条件: 上記の各操作の直後に Ctrl+Z する。3D レイヤーのクリップに「エフェクトを貼り付け」する。
- 影響: Ctrl+Z がその操作ではなく 1 つ前の別の編集を取り消す (今回の変更もまとめて消える)。redo しても戻らない。3D の配置が警告なしに失われる。マルチカム適用ではロック中の V1 も置き換えられ、旧クリップの roto が新しいクリップに付く。
- 確度: 確認済み (setClipMotion / TimelineTrack::addClip が undo を積まないことを確認)
- 修正方針: 各操作の最後に saveUndoState (マルチカムは captureExternalTrackStateForCompoundEdit + ロック確認) を入れる。pasteEffects は貼り付け先の 3D フィールドを保持する。

### [Medium] 多数のメニュー項目が、入力を受け取ったあと何もせずに「追加しました / 適用しました」と表示する
- 区画: part06
- 場所: src/MainWindow.cpp:12301-12309 (テキスト / テロップ追加)、:12369-12401 (字幕をインポート: ローカルコピーに足して捨てる)、:12403-12420 (テキストテンプレート保存: 空の既定値を保存)、:12575-12596 (画像追加 / PiP)、:12688-12702 (プラグインエフェクト)、:13265-13272 (字幕生成の「クリップに適用」)、:14878-14899 (テキストアニメ)、:16553-16575 (トランスフォームキーフレーム)、:16726-16744 (エクスプレッション)、:19633-19651 (GPU シェーダー)、:21553-21889 (パステキスト / レンジ・ウィグリーセレクター / 3D テキスト / マスク reveal / ベンド / スコープ / 可変フォント / Mograph: オブジェクトを m_pathTexts 等に溜めるだけで描画も保存もされない)、:21892-21939 (スマートリフレーム: 黒い dummyFrame を解析)
- 問題: ダイアログの結果がタイムラインにもプロジェクトにも反映されないのに、ステータスバーには成功メッセージを出す。挿入メニューの「テキスト / テロップ追加」「字幕をインポート」「画像 / 静止画を追加」「ピクチャー・イン・ピクチャー追加」(README で実装済みとされる機能) が該当する。
- 発生条件: 該当メニューを実行する (MCP の run_command からも実行できる)。
- 影響: ユーザーや AI エージェントが「適用済み」と誤認し、書き出し結果に何も入らない。TextMaskReveal / MographText は親なしで new され、溜まり続ける。
- 確度: 確認済み (各関数の本体を読み、結果を書き込む処理が無いことを確認。TODO コメントあり: :12306)
- 修正方針: 実装済みの経路 (addTextOverlayToFirstVideoClip、importMedia など) に接続するか、未実装の項目をメニューから外す / 無効化して「未実装」と表示する。

### [Medium] パーティクル追加とプラナートラッカーが、クリップ全長のフレームをすべてメモリに展開する
- 区画: part06
- 統合メモ: part08 の同旨指摘(パーティクル生成は 1080p30・60 秒で約 15GB)を統合。
- 場所: src/MainWindow.cpp:14706-14714 (addParticleEffect → ParticleSystem::renderParticleSequence が QVector<QImage> を返す、ParticleSystem.cpp:398-407)、:16198-16225 (openPlanarTrackerDialog の loadPlanarFrames)
- 問題: パーティクルは選択クリップの effectiveDuration × fps 枚をキャンバス解像度で一度に生成し、PNG に書く前に全部保持する。上限が無い。プラナートラッカーも同様に renderFrameAt の結果を全フレーム QList に積む (int の上限しか見ていない)。
- 発生条件: 長いクリップ (例: 10 分、1080p、30fps = 18000 枚 × 約 8MB) を選んでパーティクルを追加する、またはプラナートラッキングの解析を始める。
- 影響: 数十〜数百 GB のメモリを要求して OOM で落ちるか、長時間 UI が固まる (GUI スレッドで実行、ffmpeg も waitForFinished(-1))。
- 確度: 確認済み (フレーム数の算出と蓄積を確認。上限なし)
- 修正方針: 1 フレームずつ生成してすぐディスクへ書く / ffmpeg の stdin へ流す。プラナーはフレーム数か解像度に上限を設けるか、逐次読み込みにする。

### [Medium] モーショントラッカーは常にファイルの先頭フレームからテンプレートを取るため、再生ヘッド位置で選んだ領域を追跡しない
- 区画: part07
- 場所: src/MotionTracker.cpp:690-801 (decodeAndTrack はシークせず、最初にデコードしたフレームで initialRect を切り出す)。呼び出しは src/MainWindow.cpp:12976-13074、時刻の割り当ては src/TrackerLink.cpp:190-192
- 問題: 領域はプレビューに表示中のフレーム (クリップの inPoint/再生ヘッド位置) 上でドラッグして選ぶが、追跡はソースファイルの 0 フレーム目から始まり、テンプレートも 0 フレーム目の同じ矩形から作る。結果のキーフレーム時刻も「ファイル先頭からのフレーム数 / fps」で、クリップの inPoint やタイムライン位置を考えない。
- 発生条件: クリップの先頭以外のフレームで対象を選ぶ、または inPoint > 0 のクリップを追跡する。
- 影響: 選んだ物体とは無関係な領域 (0 フレーム目の同じ位置) を追跡し、オーバーレイがずれる / 時間軸もずれる。
- 確度: 確認済み
- 修正方針: 開始ソース時刻 (clip.inPoint + 再生ヘッドの局所時刻) を startTracking に渡し、そこへシークしてテンプレートを取る。TrackingResult.startFrame にその値を入れ、TrackerLink 側でタイムライン時刻へ変換する。

### [Medium] スタビライザーのキーフレーム時刻がストリームの生 PTS (start_time を含む) で、プレビュー側の start_time 差し引き済みソース時刻と食い違う
- 区画: part07
- 場所: src/MotionStabilizer.cpp:531-540, 566-574 (timeUs = av_rescale_q(best_effort_timestamp))。参照側は src/VideoPlayer.cpp:3916-3926 (srcSec = clipIn + ...) と src/GLPreview.cpp:4094-4130、VideoPlayer は start_time を差し引く (src/VideoPlayer.cpp:5720-5732)
- 問題: キーフレームは絶対 PTS、照合する sourceUs は start_time 基準の 0 起点時刻なので、start_time ≠ 0 の素材では start_time 分ずれたキーフレームの補正が適用される。
- 発生条件: start_time が 0 でない素材 (MPEG-TS/M2TS/AVCHD は通常 1 秒以上、edit list や B フレーム遅延のある MP4/MOV でも数十 ms) をスタビライズする。
- 影響: 別のフレームの手ぶれ補正量が当たり、揺れが消えないどころか増える。
- 確度: 確認済み (start_time の扱いの差を両側で確認)
- 修正方針: analyzeFile で stream->start_time (無ければ最初のフレームの PTS) を引いた時刻を timeUs に入れる。

### [Medium] 音声ノイズ除去 (denoiseAudio) が映像付きクリップでは提示される出力形式 (.wav/.mp3/.aac) で必ず失敗する
- 区画: part07
- 場所: src/NoiseReduction.cpp:488-503 (フィルタしない映像ストリームをパススルーで追加)、:409-411 (エンコーダは入力のコーデック)、:521-524。呼び出しは src/MainWindow.cpp:13138-13161 (V1 先頭の映像クリップ、既定名 `*_denoised.wav`)
- 問題: 音声だけを処理するときも映像ストリームを出力にコピーする。WAV / MP3 / ADTS(AAC) マクサーは音声 1 本しか受け付けないため avformat_write_header が失敗する。さらに音声エンコーダは入力コーデック (多くは AAC) を使うので、.wav/.mp3 では形式とも合わない。
- 発生条件: 「Audio Noise Reduction」を映像クリップに実行し、ダイアログの既定 (.wav) か .mp3/.aac で保存する。
- 影響: 常に「Failed to write output header」で失敗し、機能が使えない。
- 確度: 確認済み (WAV/MP3/ADTS マクサーの 1 ストリーム制約は FFmpeg の仕様)
- 修正方針: 音声のみ処理するときは映像/その他ストリームをコピーしない。出力拡張子から音声エンコーダを選ぶ (avformat_guess_codec / oformat->audio_codec) か、aformat で sample_fmt を合わせる。

### [Medium] NoiseReduction の出力でタイムスタンプが壊れる (音声フレームの PTS 単位の取り違えと、パススルーパケットの再スケール漏れ)
- 区画: part07
- 場所: src/NoiseReduction.cpp:283-288 / 424 / 613-616 (音声)、:497-501 / 641-644 (パススルー)
- 問題: abuffer の time_base はストリームの time_base だが、エンコーダの time_base は 1/sample_rate で、フィルタ出力の pts をそのままエンコーダに渡す。パススルーのパケットは入力ストリームの time_base のまま av_interleaved_write_frame し、av_packet_rescale_ts をしない (write_header でマクサーが出力側 time_base を変えることがある)。
- 発生条件: 入力の音声 time_base が 1/sample_rate でない (MKV は 1/1000、MPEG-TS は 1/90000)、または出力マクサーがパススルーストリームの time_base を変える (例: MKV 入力→MP4 出力の映像は 1/1000→1/16000)。
- 影響: 音声が数十倍の速さ/遅さのタイムスタンプになり、A/V がずれる、またはファイルが壊れる。映像のノイズ除去では音声がパススルーになるため、MKV 素材から MP4 へ出すと音ずれする。
- 確度: 可能性あり (コード上の単位の不一致は確認済み。実際のずれ量はマクサーの time_base 選択に依存)
- 修正方針: フィルタ出力の pts を av_rescale_q(buffersink time_base → encCtx->time_base) で変換する。パススルーでは av_packet_rescale_ts(in_tb, out_tb) を行う。

### [Medium] パス列挙 (MediaPaths) が renderInPlaceOriginal とメディアプールの素材を含まず、オフライン検出・再リンク・プロジェクト収集から漏れる
- 区画: part07
- 場所: src/MediaPaths.cpp:47-59, 152-171 (enumeratePathSlots)。利用側は src/MainWindow.cpp:9254 / 9523 / 9532 (missingFiles)、src/ProjectCollector.cpp:155、src/MainWindow.cpp:10402 (取り込みハブ)。Timeline::relinkMediaPaths (src/Timeline.cpp:8367-) も同じ 2 つを書き換えない
- 問題: ClipInfo::renderInPlaceOriginal (保存される。src/ProjectFile.cpp:2091-2094) の filePath / lutFilePath と、ProjectData::mediaPool の MediaAsset::filePath が列挙対象に無い。入れ子シーケンスの JSON 内の renderInPlaceOriginal も見ない。
- 発生条件: Render in Place したクリップ、またはメディアプールにだけある素材があるプロジェクトで、素材を移動して開く / 再リンクする / 「プロジェクトを収集」する。
- 影響: 欠落ダイアログに出ず再リンクできない。収集したプロジェクトでは元素材がコピーされず元の絶対パスを指したままになり、別マシンで Render in Place の分解 (元に戻す) やプールからの再利用ができない。
- 確度: 確認済み
- 修正方針: enumeratePathSlots に renderInPlaceOriginal (入れ子 JSON 含む) と mediaPool の資産を PathSlot として追加し、Timeline::relinkMediaPaths とプールにも同じ置換を適用する。

### [Medium] モバイル書き出しが端末の上限値をそのまま出力設定にし、SDR のタイムラインを HDR10 で出す / fps を上げる / ビットレートを最大にする
- 区画: part07
- 場所: src/MobilePreset.cpp:52, 71, 85, 99-104
- 問題: cfg.fps = device.maxFrameRate (iPhone/Galaxy 等は 60)、videoBitrate = device.maxVideoBitrateKbps (72〜120 Mbps)、hdr10 = device.supportsHdr (10 機種中 8 機種が true) をタイムラインの内容と無関係に設定する。書き出し側 (src/libavcore/Encode.cpp:559-563、src/RenderQueue.cpp:1572-1573) は hdr10 で BT.2020/PQ のタグと 10bit を付けるが、SDR→PQ の画素変換は書き出し経路に見当たらない。
- 発生条件: モバイル書き出しで Generic 以外の端末を選ぶ (24/25/30fps の SDR プロジェクトが通常)。
- 影響: SDR 映像が PQ として解釈され色と明るさが大きく狂う。24fps が 60fps に変換されてカクつく。ファイルが不必要に巨大になる。
- 確度: 確認済み (設定値の流れ)。PQ 変換が無いことは grep による (可能性ありの要素)
- 修正方針: fps はプロジェクト fps を上限で clamp、ビットレートは解像度/fps から決めて上限で clamp、HDR10 はタイムラインが HDR のときだけ有効にする。

### [Medium] モバイル書き出しで縦長 (9:16) の素材が横長の上限で縮小され、ダイアログの元サイズも最初に開いた時点のまま更新されない
- 区画: part07
- 場所: src/MobilePreset.cpp:38-43、src/MainWindow.cpp:17803-17810 (`if (!m_mobileExportDialog)` で 1 回だけ生成)、src/MobileExportDialog.cpp:21, 170
- 問題: 端末の maxResolution はすべて横長 (3840x2160 / 1920x1080) で、縦長ソースにも scale = min(maxW/srcW, maxH/srcH) を使う。Generic 端末 (既定のフォールバック) では 1080x1920 が 606x1080 になる。また MobileExportDialog は初回に m_projectConfig のサイズで作られ、以降プロジェクトの解像度を変えても m_sourceSize が古いまま。
- 発生条件: 縦長プロジェクトを Generic iOS/Android で書き出す。あるいはダイアログを一度開いた後にプロジェクト解像度を変える / 別プロジェクトを開く。
- 影響: 解像度が約半分になる。古いアスペクトで出力されて引き伸ばしや黒帯が出る。
- 確度: 確認済み
- 修正方針: 縦長ソースでは上限を回転 (短辺/長辺で比較) してから縮尺を決める。ダイアログを開くたびに元サイズを渡し直す (setSourceSize を追加する)。

### [Medium] ノードコンポジットが機能しない: ノード型が nodelib::NodeRegistry にしか登録されず、NodeGraph::addNode はポートもパラメータも無いノードを作る
- 区画: part07
- 場所: src/NodeLibrary.cpp:657-883 (nodelib の registry だけに登録)、src/NodeGraph.cpp:19-33, 131-148, 170-175 (NodeGraph 独自の registry。登録済みは "Output" だけ)、src/NodeCanvasWidget.cpp:345-355 / 467、src/LayerNodeBridge.cpp:125-160、src/NodeEvaluator.cpp:159-172 (setNodeEvaluatorFn はどこからも呼ばれず、evaluateBuiltinNode も使われない)
- 問題: NodeGraph::registerNodeType を呼ぶ箇所が無いので、"SolidColor" / "ImageInput" / "GaussianBlur" などのノードは inputs/outputs/params が空で作られる。そのため graph.connect は常に失敗し (fromEffectStack の連結も全滅)、プロパティパネルも空になる。評価器はどのノードにも値を返さず、onNodeGraphChanged (src/MainWindow.cpp:22152-22166) は透明画像を GLPreview に表示する。
- 発生条件: ノードコンポジットモードを ON にする / ノードを追加する / パラメータを変える。
- 影響: ノード同士を接続できず効果も出ない。プレビューが透明 (黒) で上書きされる。OFF にすると isLinearChain が偽になり、エフェクトスタックへ書き戻されない。
- 確度: 確認済み
- 修正方針: registerBuiltinNodes で NodeGraph::registerNodeType にも登録する (registry を 1 つにまとめる)。NodeEvaluator に nodelib::evaluateBuiltinNode を設定する。

### [Medium] ネットワークレンダーサーバが全インタフェースで認証なしに待ち受け、任意の接続元がジョブの完了/失敗を偽装できる。キャンセルも効かない
- 区画: part07
- 場所: src/NetworkRender.cpp:91 (QHostAddress::Any)、:346-360 / 398-506 (送信元ソケットや割当ノードを確認しない)、:140-163 (cancelJob)、:749-758 (クライアントは CANCEL_JOB を処理しない)
- 問題: (1) JOB_PROGRESS / JOB_COMPLETE / JOB_FAILED は HELLO 前のソケットからも受け付け、jobId が一致すれば状態を変え、最後のセグメントが「完了」すると assembleSegments で出力を上書きする。HELLO したノードにはプロジェクトや出力の絶対パスが送られる。HELLO 前のソケットはハートビートの切断対象外で、改行の無いデータで m_readBuffers が無制限に増える。同じ nodeId で再 HELLO すると、古いソケットの切断で新しいノードが消される。(2) 分割ジョブのマスターをキャンセルしても子セグメントは Pending のまま割り当て・完了し、assembleSegments でマスターが Complete に戻る。単一ジョブのキャンセル後に遅れて届いた JOB_COMPLETE も Failed→Complete に上書きし、別ジョブ中のノードを Idle にする。(3) handleJobFailed / cancelJob は checkAllComplete を呼ばないので、最後のジョブが失敗すると allJobsComplete が出ない。
- 発生条件: サーバを起動した状態で同じ LAN の誰かが TCP 接続する。ジョブをキャンセルする / 失敗させる。
- 影響: 出力ファイルの破損や上書き、内部パスの漏洩、メモリ枯渇。キャンセルしたジョブが完了扱いになる。なお NetworkRenderClient は本体から使われておらずレンダリング処理も無いため、正規のノードでジョブが処理されることは無い (Add Job の範囲も 0–999 フレーム固定。src/NetworkRender.cpp:962-963)。
- 確度: 確認済み
- 修正方針: 既定は localhost のみで待ち受け、共有トークンで HELLO を認証する。進捗/完了/失敗はその jobId の assignedNode のソケットからだけ受け付ける。キャンセルは子セグメントへも伝え、状態遷移は Assigned/Rendering からだけ許す。失敗時にも checkAllComplete を呼ぶ。

### [Medium] スタビライズの結果 (stabilizerKeyframes) がプロジェクトに保存されず、開き直すと消える
- 区画: part08
- 場所: src/ProjectFile.cpp:1948-2100 (`clipToJson`)、src/ProjectFile.cpp:2102-2247 (`clipFromJson`)。設定側は src/MainWindow.cpp:21242 → src/Timeline.cpp:17388-17401 (`setClipStabilizerKeyframes`)
- 問題: `ClipInfo::stabilizerKeyframes` (Timeline.h:385) は「編集 > スタビライズ」で V1 先頭クリップに書き込まれ、プレビュー (VideoPlayer.cpp:3916 / :4729) と SmartRender の判定 (SmartRender.cpp:444 / :688) で使われます。しかし `clipToJson` / `clipFromJson` にはこのキーがなく、ProjectData にも別枠の保存先はありません (ProjectFile.* を grep して確認)。
- 発生条件: スタビライズを実行してから保存し、プロジェクトを開き直す (自動保存からの復元も同じです)。
- 影響: 解析結果が黙って失われ、開き直すと手ぶれ補正がかかっていない状態に戻ります。エラーも警告も出ません。
- 確度: 確認済み (書き込み経路、clipToJson / clipFromJson の全キー、ProjectData のフィールドを読みました。selftest は実行していません)
- 修正方針: clipToJson に `stabilizerKeyframes` (空なら省略) を追加し、clipFromJson で読み戻す。renderInPlaceOriginal の 1 段にも同じ処理を通す。

### [Medium] プラナートラッカーの「ダンピング」が測定値の追従率として使われ、既定 30% では毎フレーム 30% しか動かないため、数 px/frame の動きで追跡が外れる
- 区画: part08
- 場所: src/PlanarTracker.cpp:1222-1227 (`newPts = old + alpha*(candidate-old)`、`alpha = dampingFactor`)。プリセットの説明は src/PlanarTrackerPreset.cpp:20 / :27
- 問題: 探索で見つけた位置 `candidate` への移動量に `dampingFactor` を掛けており、ダンピングの値が大きいほど追従が強くなります。プリセットの説明は逆の意味です (precise-feature「damping 弱」= 0.1、robust-motion「高 damping でジッタ吸収」= 0.5)。等速 v px/frame で動く対象では、定常的な遅れ L = (1-α)v/α が残ります。探索は遅れた位置の周り ±searchRadius で行うため、L+v が探索半径を超えると対象を見失います。
- 発生条件: 既定 (α=0.3、半径 16) では v ≳ 4.8 px/frame で外れます。「精密特徴追跡」プリセット (α=0.1、半径 8) では v ≳ 0.8 px/frame で外れます。
- 影響: 追跡結果のコーナーが対象から遅れたまま、または離れたままになります。それをもとにした「3D カメラを解析」の姿勢も誤ります。信頼度は SAD から計算されるため、外れても警告になりにくいです。
- 確度: 確認済み (trackNextFrame / refinePoint の式と、プリセットの値・説明を読み、上の遅れの式を手計算しました。実映像では試していません)
- 修正方針: 測定は candidate をそのまま採り、平滑化は `new = candidate + d*(old_smoothed - candidate)` のように「d が大きいほど前の値を残す」形にする。または UI とプリセット説明の意味を実装に合わせ、既定値を 1 に近づける。

### [Medium] タイムリマップの「オプティカルフロー」補間で、中間フレームの被写体が逆向きに動く
- 区画: part08
- 場所: src/TimeRemap.cpp:266-269 (`warpImage(a, flow, frac)`)。API の契約は src/OpticalFlow.h:22-25 / :52-58、src/OpticalFlow.cpp:218-245 / :324-326
- 問題: `estimateFlow` は a→b の順方向の変位を返します (SAD で a の (bx,by) のブロックを b の (bx+dx,by+dy) と照合)。一方 `warpImage` は `out(x) = src(x + t*flow)` という引き寄せ型です。そのため `warpImage(a, flow, frac)` では、a の x0 にあった被写体が x0 - frac*d に描かれます。正しい位置は x0 + frac*d です。同じ API を使う Overlay.cpp:91 (モーフカット) と TimelineFrameRenderer.cpp:1004 は、符号を反転してから渡しています。
- 発生条件: タイムリマップのダイアログでフレーム補間に OpticalFlow を選び (TimeRemapDialog.cpp:467)、スローや非整数のソースフレームで再生・書き出しする。
- 影響: 整数フレームの間で被写体が一度後ろへ戻ってから次のフレームへ飛ぶため、ガタついた動きになります。プレビューにも書き出しにも出ます。selftest (effects_selftests.cpp:611-625) は前景の画素数しか見ていないため、検出できません。
- 確度: 確認済み (変位の定義と warpImage の式から、一様並進の場合を手で追いました。TimeRemap.cpp は part08 の範囲外ですが、OpticalFlow の契約を追う過程で見つけました)
- 修正方針: TimeRemap 側で flow を反転してから渡す (`warpImage(a, -flow, frac)`)。可能なら Overlay と同じく a→b と b→a の両方向でワープして混ぜる。

### [Medium] パーティクル効果ダイアログの初期値がスピンの最小値 (発生率 0 など) のままで、そのまま OK すると何も映らないクリップが作られる。パラメータを変えてもプレビューが更新されない
- 区画: part08
- 場所: src/ParticleEffectDialog.cpp:18-26 (コンストラクタが setConfig を呼ばない)、:54-58 (プリセットを入れてから currentIndexChanged を connect)、:91-94 (emitRate の範囲 0..10000)、:597 以降の connect 一覧 (色・力場以外のスピンに `onParamChanged` が繋がっていない)。呼び出し側は src/MainWindow.cpp:14693-14714
- 問題: 新規に開いたとき、`setConfig` が一度も呼ばれません。プリセットのコンボは「Bouncing Confetti」を表示しますが、各スピンは既定値 0 を範囲の最小値に丸めた値 (emitRate=0、life=0.01、size=0.1、turbulenceScale=0.1 など) のままです。さらに、スピンの valueChanged は `onParamChanged` に接続されていないため、値を変えてもプレビューは再計算されません (更新されるのは色と力場の変更時だけです)。
- 発生条件: クリップに既存のパーティクル設定がない状態で「パーティクル効果」を開き、プリセットを選び直さずに OK を押す。
- 影響: MainWindow は透明フレームを `durationSec * fps` 枚生成して ffmpeg でクリップにし、エラーも出しません。表示中のプリセットと実際に使われる設定が食い違います。
- 確度: 確認済み (コンストラクタ、setupPresetRow、すべての connect を読みました)
- 修正方針: コンストラクタの最後で現在のプリセットを `setConfig` する。全スピン・チェック・コンボの変更を `onParamChanged` に接続する。

### [Medium] 「プロジェクトを収集」が、収集先のメディアを絶対パスで書き込むため、収集したフォルダを移動・受け渡しすると全メディアがオフラインになる
- 区画: part08
- 場所: src/ProjectCollector.cpp:240 (`ps.setValue(QDir::cleanPath(destPath))` で絶対パスを書く)。契約のコメントは src/ProjectCollector.h:19-22 (「destDir-relative "media/<basename>"」に書き換えると説明)
- 問題: ヘッダには、`media/<basename>` の相対パスに書き換えると書かれています。しかし実装は、収集時点の絶対パスを書きます。ロード側 (ProjectFile::load / MainWindow) には、プロジェクトのフォルダを基準に相対パスを解決する処理がありません (`isRelative` などを grep しても見つかりませんでした)。
- 発生条件: 収集したフォルダを別の場所や別の PC にコピーし、`project.veditor` を開く。
- 影響: 収集の主な目的 (アーカイブや受け渡し) が果たせず、すべてのクリップ・LUT・オーバーレイが「オフライン」になります。手動で再リンクすることは可能です。
- 確度: 確認済み (書き込み箇所と、ロード時にパスをそのまま開くことを確認しました)
- 修正方針: 保存時に projectDir 相対 (`media/...`) で書き、読み込み時に相対パスなら .veditor のあるフォルダを基準に解決する。または、絶対パスが見つからないときに `<projectDir>/media/<basename>` を探すフォールバックを入れる。

### [Medium] OBS の録画を取り込むと、配置の計算 (トラック・開始時刻) が捨てられ、音声トラック別ファイルやリプレイバッファまで V1 に一列に並ぶ
- 区画: part08
- 場所: src/MainWindow.cpp:17902-17910 (`p.trackIndex` / `p.startMs` を無視して `addClip(p.filePath)` を順に呼ぶ)、src/ObsLayout.cpp:47-72 (音声 1..N とソースレコード 3.. のトラック番号が重なる)、:76-84 (リプレイバッファを track 0 の、主映像と同じ startMs に置く)、src/ObsScanner.cpp:170-180 (ソースレコードとリプレイは basename ごとに別のグループになり、durationSec が 0 のまま)
- 問題: `layoutToTimeline` が返すトラック番号と開始時刻を、取り込み側がまったく使っていません。さらに、レイアウト自体も壊れています。音声ファイルが 3 本以上あるとソースレコードと同じトラック・同じ時刻に重なり、リプレイバッファは主映像に重なります。
- 発生条件: 取り込みハブの OBS タブで、`-track1.aac` などの多重音声やリプレイバッファを含むフォルダをスキャンし、「タイムラインへ」を押す。
- 影響: 映像のあとに同じ収録の音声ファイルが V1 へ続けて並び、尺も同期も崩れたタイムラインができます。ステータスバーは「N 件追加」と成功を表示します。
- 確度: 確認済み (scan → layout → MainWindow の取り込み処理まで読みました)
- 修正方針: 取り込み側で `trackIndex` / `startMs` を使って配置する (音声は A トラックへ)。レイアウトは、音声トラックとソースレコードのトラック番号の範囲を分ける。リプレイは元セッションの後ろに置く。スキャナは `<Source>_<timestamp>` を同じタイムスタンプのセッションに紐付けて、尺も probe する。

### [Medium] Stale になったプロキシが「プロキシ生成」でも自動プロキシでも作り直されず、状態表示が「Generating proxy files...」のまま止まる
- 区画: part09
- 場所: src/ProxyManager.cpp:393-397, 427-431, 443-445(呼び出し側 src/MainWindow.cpp:14078-14172, src/Timeline.cpp:5961-5967)
- 問題: 設定のフィンガープリントが変わった、またはソースが更新されたプロキシは `isEntryStale()` が true になります。このため `hasProxy()` は false を返しますが、エントリの status は `Ready` のままです。`generateProxy` と `generateAllProxies` は status が `Ready` なら何もせず抜けます。MainWindow はこのクリップを `hasProxy()==false` と見て再生成の確認を出さず、`proxyQueuePaths` に積んで `generateAllProxies` を呼びます。ところがキューは 0 件のままです。さらに `m_totalQueued == 0` だと `processNextInQueue()` が呼ばれないので、`allProxiesReady` も発火しません。
- 発生条件: ソース動画を上書き更新する、またはビルドの変更で使うエンコーダが変わる → メニューの「プロキシ生成」を実行する、またはクリップを読み込んで自動プロキシが動く。
- 影響: プロキシが再生成されないまま、プレビューは黙って元の重いメディアで再生されます(getProxyPath も stale を見て元のパスを返します)。状態バーは「Generating proxy files...」のまま変わりません。管理ダイアログから 1 件ずつ「再生成」しない限り直りません。
- 確度: 確認済み
- 修正方針: generateProxy / generateAllProxies の判定を `status==Ready && !isEntryStale(entry)` にする(stale なら古いファイルを消してキューに積む)。キューが 0 件のときは `allProxiesReady` を出す。

### [Medium] Python スクリプトの `veditor.*` API を呼ぶと、5 分のタイムアウトまで止まる
- 区画: part09
- 場所: src/PythonScript.cpp:222-226, 240-254(ホスト側 451-472)
- 問題: ヘルパの `_api_call` は `sys.stdout.write(req)` で要求を送ります。ところが、その前に `sys.stdout = _Tee(...)` に差し替えられているため、要求は `{"type":"output","text":"{\"type\":\"api_call\"...}"}` という出力メッセージに包まれて届きます。ホストはこれを出力として表示するだけで応答を返しません。Python 側は `sys.stdin.readline()` で応答を待ち続けます。
- 発生条件: スクリプトで `veditor.log('x')` など、API を 1 つでも呼ぶ(API リファレンスとプレースホルダの例そのもの)。
- 影響: スクリプトが `kScriptExecutionTimeoutMs`(5 分)まで止まり、そのあと "Script timed out." で失敗します。API 機能が実質的に使えません。なお `ScriptAPI::registerAll()` もどこからも呼ばれていません。
- 確度: 確認済み
- 修正方針: `_api_call` で `_real_stdout` に書く(定義を差し替えより後にするか、`_real_stdout` を先に保存しておく)。

### [Medium] Remotion 書き出しがクリップ前の隙間(leadInSec)を無視し、隙間より後のクリップが前に詰まる
- 区画: part09
- 場所: src/RemotionExport.cpp:336-354, 404-479, 511-549, 735-819, 849-880
- 問題: 各トラックの開始フレームを `offset += clip.effectiveDuration()` で積み上げていて、`ClipInfo::leadInSec`(タイムライン上の前の隙間)を足していません。`calculateDurationInFrames` も同様です。TimelineSequence::duration() は `leadInSec` を足しています。
- 発生条件: クリップの間に隙間がある、またはトリムで leadInSec が付いたクリップがあるプロジェクトを Remotion へ書き出す。
- 影響: 書き出された Video.tsx / timeline.ts で、隙間より後のクリップがすべて早いタイミングに置かれます。音声トラックと映像トラックの同期も崩れ、全体の尺も短くなります。
- 確度: 確認済み
- 修正方針: offset に `qMax(0.0, clip.leadInSec)` を加えてから startFrame を計算する。尺の計算でも同じく加える。

### [Medium] Remotion のアセットコピーで、同じファイル名の別素材が上書きされず、黙って取り違えられる
- 区画: part09
- 場所: src/RemotionExport.cpp:1313-1338(参照側 416-418, 503-506)
- 問題: コピー先が `assets/<fileName>` だけで決まり、`if (!QFile::exists(dest))` のときしかコピーしません。別のフォルダにある同名ファイル(カメラの `C0001.MP4` など)は 2 本目がコピーされません。それでいて TSX からは同じ `./assets/C0001.MP4` を参照します。同じ出力先へ再書き出しした場合も、前回の古いアセットがそのまま使われます。
- 発生条件: 別フォルダの同名メディアを 2 本以上使う。または、素材を差し替えてから同じ出力先へ書き出し直す。
- 影響: 書き出し先の Remotion プロジェクトで、別のクリップの映像や音声が再生されます。エラーは出ません。
- 確度: 確認済み
- 修正方針: 衝突したらハッシュや連番の付いた名前にして、その対応表で src を出力する。既存ファイルはサイズと mtime を比べて更新する。

### [Medium] RenderQueue で開始だけを指定した範囲(終了は「末尾まで」)だと、開始位置からタイムライン全体の長さを書き出してしまう
- 区画: part09
- 場所: src/RenderQueue.cpp:1500-1528, 2046-2074(UI は src/RenderQueueDialog.cpp:124-133, 167-171)
- 問題: `totalFrames` の初期値はタイムライン全長のフレーム数です。`endUs > startUs` のときしか範囲で上書きしません。`startUs > 0, endUs == 0` のときは、開始位置から全長ぶんのフレームを描画します。startSequenceExport(1320 行)は `total - start` を正しく引いているので、経路によって結果が食い違います。`startUs >= endUs > 0` の場合も全長になります。
- 発生条件: レンダーキューの「ジョブ追加」で、開始を 10000 ms、終了を「(末尾まで)」にして実行する。
- 影響: 出力の末尾に、開始秒ぶんの余分な黒(またはタイムライン外)のフレームが付きます。書き出し時間も無駄に延びます。
- 確度: 確認済み
- 修正方針: 終了が未指定なら `totalFrames = timelineTotalFrames - round(startUs*fps/1e6)` とする。start >= end のときはジョブを拒否する。

### [Medium] 画面録画で ffmpeg が起動直後にエラー終了しても「Recording saved」と表示される
- 区画: part09
- 場所: src/ScreenRecorder.cpp:208-221, 119-123(MainWindow.cpp:14502-14507)
- 問題: `finished` ハンドラは exitCode も exitStatus も見ずに、必ず `recordingStopped(outputPath)` を出します。Windows では音声の取り込みが既定で ON になっており、`-f dshow -i audio=virtual-audio-capturer` を付けます。このデバイスは別途ツールを入れないと存在しないので、ffmpeg はすぐに非 0 で終了します。exit code による終了は errorOccurred にもなりません。また MainWindow は lambda に `Qt::UniqueConnection` を付けて接続していますが、lambda には効かないため、録画を開始するたびに接続が増えていきます。
- 発生条件: 標準的な Windows 環境で画面録画を開始する(ほかにも、存在しないデバイスや書き込めない出力先などで ffmpeg が失敗した場合)。
- 影響: 録画は開始直後に止まっているのに「Recording saved: …」と表示され、ファイルは無いか空です。2 回目以降はエラーのメッセージボックスが重複して出ます。
- 確度: 確認済み
- 修正方針: finished で exitCode != 0 なら stderr の末尾を付けて recordingError を出す。dshow の音声デバイスは列挙して確認し、無ければ音声なしにする。lambda の接続は一度だけにする。

### [Medium] ロトツールで「Brush mode」を入れただけで OK すると、クリップ全体が透明になる
- 区画: part09
- 場所: src/RotoToolsDialog.cpp:680-686, 745-767, 601-606 / src/RotoApply.cpp:56-60 / 保存側 src/MainWindow.cpp:15056-15058
- 問題: `ensureBrushMask()` は、Brush mode のチェックや Clear を押しただけで、全面 0 の Grayscale8 マスクを作ります。`brushMask()` はそれを返し、MainWindow は null でなければ `entry.brushMask` に保存します。パスもキーフレームも無いクリップでは、`applyEntryToFrame` が `combineMasksMax(null, brush)` で全面 0 のマスクを作り、`applyToFrame` が全画素の alpha を 0 にします。
- 発生条件: ロトを持たないクリップでロトツールを開き、Brush mode を ON にする(または Clear を押す)だけで、何も描かずに OK を押す。
- 影響: クリップがプレビューでも書き出しでも完全に消えます(下のトラックか黒が見える)。原因に気づきにくいです。
- 確度: 確認済み
- 修正方針: 一度も描画していない、または全面 0 のブラシマスクは返さない(保存しない)。あるいは、ブラシは既存のマスクへの加算・減算の補正としてのみ扱う。

### [Medium] プロキシ表示中に作ったロトのパスはプロキシのピクセル座標で保存され、書き出しでは元解像度にそのまま当てられる
- 区画: part09
- 場所: src/RotoToolsDialog.cpp:123-130, 576-580(フレームのピクセル座標で編集)/ src/RotoApply.cpp:50-54(`renderMask(…, frame.size())`)/ 入力フレーム src/MainWindow.cpp:8540-8544(getProxyPath でデコード)/ 書き出し src/TimelineFrameRenderer.cpp:1847-1869(元メディアのフレーム)
- 問題: ロトツールに渡すフレームは `ProxyManager::getProxyPath` でデコードしているので、プロキシモードでは 640x360 になります。RotoPath はその画素座標(と feather の px)で保存されます。書き出しでは、元解像度(たとえば 1920x1080)のフレームに同じ座標でマスクを描きます。正規化した座標への変換はどこにもありません。プロキシは常に 640x360 なので、16:9 以外の素材では縦横比もずれます。
- 発生条件: プロキシモードが ON の状態でロトを作成または編集し、書き出す(逆に、OFF で作って ON でプレビューした場合も同様)。
- 影響: 書き出しでマスクが左上の 1/3 程度の範囲に縮み、被写体の大部分が切り抜かれます。プレビューと書き出しが一致しません。
- 確度: 確認済み
- 修正方針: ロトの座標をソースのネイティブ解像度(または 0..1 の正規化座標)で保存し、適用時にフレームのサイズへ換算する。ダイアログには元メディアをデコードしたフレームを渡す。

### [Medium] 無音カットで分割した 2 片目以降に、キーフレーム・エフェクト時間窓・音量エンベロープ・3D テキスト時刻・トランジションがずれずにコピーされる
- 区画: part10
- 場所: src/SilenceCut.cpp:155-178 (planKeepClips)、呼び出し元 src/Timeline.cpp:9889-9946 (applySilenceCutToClip)
- 問題: planKeepClips は `ClipInfo c = src;` で元クリップを丸ごとコピーし、inPoint/outPoint だけを書き換える。通常の分割 (TimelineTrack::splitClipAt, Timeline.cpp:2125-2196) が右片に行っている「キーフレーム / string track / effect startSec・endSec / volumeEnvelope / text3DConfig.timeOffsetSec を分割位置だけずらす」「leadIn・trailOut を解除する」「timeRemap・speedRamp を切り出す」処理が一切ない。呼び出し元も leadInSec を 0 にするだけ。
- 発生条件: キーフレーム、時間窓付きエフェクト、音量エンベロープ、フェード (leadIn/trailOut)、3D テキスト、速度ランプ/タイムリマップのいずれかを持つクリップに無音カットをかける。
- 影響: 2 片目以降でアニメーションと音量カーブが元のクリップ先頭基準で再生され、見た目と音がカット前と変わる。フェードイン/アウトが全片の先頭/末尾に複製される。README が「分割の全経路に効く」とする US-1807 の規則が、この経路では守られていない。
- 確度: 確認済み (planKeepClips → applySilenceCutToClip → setClips と追い、シフト処理が無いことを確認)
- 修正方針: 片ごとに splitClipAt と同じ右片補正 (共通関数へ切り出す) を通す。または TimelineTrack::splitClipAt を繰り返し呼んでから無音片を削除する実装に替える。

### [Medium] 字幕翻訳で Google API キーがエラーダイアログに表示され、環境変数のキーも平文で設定に保存される
- 区画: part10
- 場所: src/SubtitleTranslator.cpp:199-200, 219-220 / src/SubtitleTranslatorDialog.cpp:62-75, 173-182, 202-205
- 問題: (1) Google V2 は `?key=<APIキー>` を URL に直接付ける。HTTP エラー (400/403/429 など) のときの QNetworkReply::errorString() は Qt 内部で "Error transferring <url().toString()> - server replied: ..." になり、クエリ部分ごと URL を含む。その文字列が QMessageBox とプレビュー欄にそのまま出る。(2) ダイアログは defaultConfig() で環境変数 VEDITOR_TRANSLATE_KEY をキー欄に入れる。「このキーを保存」が既定で ON なので、翻訳を押すと環境変数で渡したキーが QSettings (Windows はレジストリ、他は ini) に平文で書かれる。チェックを外しても、以前に保存したキーは消えない。
- 発生条件: (1) キーが無効、クォータ超過などで Google がエラーを返したとき。(2) 環境変数でキーを渡し、既定のまま翻訳を実行したとき。
- 影響: 画面共有やスクリーンショットで API キーが漏れる。環境変数で渡したつもりの秘密がディスクに残る。
- 確度: 確認済み (エラー文言の形式は Qt の QNetworkReplyHttpImpl の実装に基づく)
- 修正方針: キーは `X-goog-api-key` ヘッダで送り、エラー表示から URL を除く (HTTP ステータスと本文の message だけを出す)。環境変数由来のキーは保存対象から外し、チェックを外したら既存のキーを remove する。

### [Medium] 字幕翻訳が普通の長さの SRT で失敗する、または壊れた訳文を返す (Google / DeepL)
- 区画: part10
- 場所: src/SubtitleTranslator.cpp:205-215 (Google), 273-281 (DeepL)
- 問題: 全キューを 1 リクエストで送っている。Google Translate v2 の q は 1 リクエスト 128 件まで、DeepL の text は 50 件までなので、それを超える SRT はリクエスト全体がエラーになる。Google は format を指定しておらず既定が html なので、訳文が `&#39;` `&quot;` `&amp;` などの HTML 実体参照のまま字幕に入る。DeepL は QUrlQuery::toString(FullyEncoded) で本文を作るが、QUrlQuery は '+' をエンコードしない。form-urlencoded では '+' が空白に戻るので、"1+1" のような字幕の '+' が消える。
- 発生条件: キューが 51 件 (DeepL) または 129 件 (Google) 以上の SRT。または ' " & + を含む字幕。
- 影響: 数分以上の動画の字幕はほぼ翻訳できない。翻訳できても記号が化ける。
- 確度: 確認済み (件数上限と既定値は各 API の公開仕様に基づく。'+' の扱いは Qt の QUrlQuery のドキュメントどおり)
- 修正方針: 件数で分割して順に送り、結果を連結する。Google には `"format":"text"` を付ける。DeepL は JSON 本文で送るか、値を QUrl::toPercentEncoding してから組み立てる。

### [Medium] 書き出しの字幕焼き込みで、日本語の字幕が折り返されず、縁取りも描かれない
- 区画: part10
- 場所: src/SubtitleTrackRenderer.cpp:44-84 (wrapText), 141-184 / 229-245 (縁取り)
- 問題: wrapText は空白 ' ' で単語に分けて折り返す。空白の無い日本語や中国語の文は 1 語として扱われ、maxWidthFraction を超えても 1 行のまま描かれる。縁取りは `QPen::setWidth(outlineW)` を設定してから drawText で同じ位置に描き、その上に本色で描き直している。QPainter::drawText はペンの幅を使わず、ペンの色で字形を塗るだけなので、縁取りは本文の下に完全に隠れて見えない。カラオケ経路も同じ。
- 発生条件: RenderQueue::paintSubtitles (RenderQueue.cpp:118-130) を通る字幕焼き込み書き出しで、空白の無い長い字幕を使う、または縁取り (既定 2px) を期待するとき。
- 影響: 長い日本語字幕が画面の外まで伸びて切れる。白文字が明るい背景で読めない。
- 確度: 確認済み
- 修正方針: 折り返しは QTextLayout (行分割規則は UAX#14) に任せる。縁取りは QPainterPath::addText で輪郭を作り、strokePath で描く。

### [Medium] 既定のショートカットと各プリセットで同じキーが 2 つの操作に割り当たり、両方とも動かない
- 区画: part10
- 統合メモ: part05 の同旨指摘(Ctrl+D / M / Ctrl+Shift+P の重複)を統合。
- 場所: src/ShortcutManager.cpp:170 (共通テーブルの Ctrl+D)、:97 (Premiere の edit.split = Ctrl+K)、:106 (Premiere の view.full_screen = Ctrl+Shift+F) / src/MainWindow.cpp:4471 (複製 Ctrl+D)、:4643 (規定トランジションを適用 Ctrl+D)、:5666 (キーフレーム編集 Ctrl+K)、:5572 (ビデオエフェクト Ctrl+Shift+F)
- 問題: ShortcutManager は登録した一部のアクションのシーケンスだけを書き換え、ほかのアクションとの衝突を調べない。既定の状態で m_duplicateAction と applyDefaultTransAct がどちらも Ctrl+D で、同じウィンドウ内にある。Premiere プリセットにすると、未登録の kfAction (Ctrl+K) と再生ヘッドで分割、また fxAction (Ctrl+Shift+F) と全画面 (view.full_screen を登録している場合) がぶつかる。ShortcutCustomizeDialog で手で割り当てたときも警告が出ない。
- 発生条件: 既定のまま Ctrl+D を押す。Premiere プリセットで Ctrl+K を押す。ダイアログで既に使われているキーを割り当てる。
- 影響: Qt は曖昧なショートカットとみなし、どちらのアクションも発火しない (activatedAmbiguously だけが出る)。
- 確度: 確認済み (setShortcut の箇所と、両方が同じメニューバー配下にあることを確認)
- 修正方針: setBinding / applyPreset で、メインウィンドウ上の全 QAction のショートカットと重複を検査し、重複を拒否するか相手を外す。既定の Ctrl+D の重複は片方を変える。

### [Medium] 速度ランプ書き出し (FFmpeg) が、ランプにならず一定速度になる
- 区画: part10
- 場所: src/SpeedRamp.cpp:397-412, 434-445, 215 / 呼び出し src/MainWindow.cpp:14306-14321
- 問題: 可変速は「隣り合う 2 点の区間ごとに平均速度を 1 つ求め、その一定速度で区間を書き出す」作りになっている。GUI は常に 2 点 (0 秒と effectiveDuration) しか置かないので、区間は 1 つしかなく、全体が (開始速度 + 終了速度) / 2 前後の一定速度で書き出される (イージングも効かない)。最初の点より前と最後の点より後のソースは区間に入らず、出力から落ちる。ピッチ非保持のときは `asetrate=44100*speed` で入力を 44.1kHz と決め打ちしているので、48kHz の素材では速度と音程が 44100/48000 倍ずれ、映像と音がずれる。ソース範囲も 0 秒から始まり、クリップの inPoint を無視する。
- 発生条件: メニューの速度ランプで開始と終了の速度を変えて書き出す。ピッチ保持を OFF にして 48kHz の素材を使う。
- 影響: 書き出しにランプがかからない。トリム済みのクリップは別の範囲が書き出される。音ずれが起きる。
- 確度: 確認済み
- 修正方針: 区間を細かく刻む (例えば 0.1 秒単位) か、setpts の式で速度の積分を直接与える。asetrate には ffprobe で得た入力サンプルレートを使う。範囲は inPoint から outPoint にする。

### [Medium] スペクトル編集で「適用」を押すたびに元の音声からやり直し、前に除去した部分が戻る
- 区画: part10
- 場所: src/SpectralEditDialog.cpp:193-221 / src/SpectrogramWidget.cpp:161-186
- 問題: onApplyClicked は毎回 m_samples (元の音声) に対して applySpectralEdit を実行し、結果で m_processedSamples を上書きする。選択できる領域は 1 つだけ (selectedRegions が 1 要素)。スペクトログラムも元の音声のまま更新されない。
- 発生条件: 1 か所を選んで適用し、続けて別の箇所を選んで適用する (RX のように複数のノイズを順に消す使い方)。
- 影響: 最後に適用した 1 領域だけが反映された WAV が保存され、それまでの修復が黙って失われる。表示からはそれが分からない。
- 確度: 確認済み (MainWindow.cpp:19295-19310 で processedSamples() を書き出すまで追跡)
- 修正方針: 適用の入力を m_processedSamples にして累積させ、適用後にスペクトログラムを処理後の音声で作り直す。元に戻す手段として元の音声は別に保持する。

### [Medium] スマートリフレームが未初期化のダミー画像を解析し、結果は使われない Exporter にだけ渡される
- 区画: part10
- 場所: src/MainWindow.cpp:21916-21930 (openSmartReframe) / src/SmartReframe.cpp:287-322 / src/SmartReframeDialog.cpp:241-253
- 問題: 解析ループは毎回 `QImage dummyFrame(w, h, Format_RGB888)` を作って analyzeFrame に渡している。この画像は fill していないので、未初期化メモリの内容から顕著度とクロップを決めることになる。キーの時刻は `clips[0].inPoint + i*step` (ソース秒) なのに、Exporter は `framePts - clip.inPoint` (クリップ内の秒) で引くので、時間軸もずれている。m_smartReframe は使い回され、analyzeFrame は m_rawTrack に追記するだけなので、2 回目の解析は 1 回目のキーと混ざる。そもそも結果を渡す m_exporter (Exporter) の startExport はどこからも呼ばれておらず、実際の書き出し (RenderQueue) には反映されない。ダイアログの出力解像度・パディング・モーション考慮も読まれない。16:9 を選ぶと出力解像度が 720x1280 (縦長) に変わる。保存時は常に m_smartReframe.toJson() (空にならない) を書くので、読み込むと毎回 Exporter にリフレームが設定される。
- 発生条件: スマートリフレームを実行する。
- 影響: ステータスバーには「解析完了」と出るが、書き出しには何も起きない。仮に経路を繋いでも、クロップは非決定的になる。
- 確度: 確認済み (startExport の呼び出しが無いことを grep で確認)
- 修正方針: 実際にデコードしたフレームで解析し、キーはクリップ内の秒で持つ。解析前に track を clear する。適用先は RenderQueue / renderFrameAt にする。16:9 の出力解像度の対応表を直す。

### [Medium] 字幕自動生成の「テキストオーバーレイとして適用」が何も適用せず、2 回目以降はダイアログが重複する
- 区画: part10
- 場所: src/MainWindow.cpp:13265-13271, 13245-13292 (SubtitleGenerator の利用側) / src/SubtitleGenerator.cpp:110-146, 341-387
- 問題: (1) `auto clips = m_timeline->videoClips();` は const 参照を値でコピーするので、addOverlay はコピーにしか効かず、タイムラインに書き戻されない。それでもステータスには「Applied N subtitle overlay(s)」と出る。(2) ラムダの connect に Qt::UniqueConnection を指定しているが、ラムダには効かない (Qt の仕様)。呼ぶたびに接続が増え、2 回目の生成では完了ダイアログが 2 回出る。(3) generate() は実行中かどうかを確認しない。2 回目を呼ぶと m_process と m_tempDir が上書きされ、1 回目の finished ハンドラが 2 回目の一時ディレクトリを消し、2 回目の QProcess を deleteLater して m_process を nullptr にする。(4) 生成結果は m_subtitleSegments に入らないので、renderSubtitleTrack (MainWindow.cpp:21944-21950) は生成直後でも常に「セグメントがありません」になる。
- 発生条件: 字幕生成で「Apply to clip」を選ぶ。または生成を 2 回実行する、生成中にもう一度実行する。
- 影響: 機能が働かないのに成功と表示される。重複実行すると結果が失われる。破棄された QProcess の finished が null の m_process を参照してクラッシュする可能性がある。
- 確度: (1)(2)(4) は確認済み。(3) の状態破壊は確認済み、クラッシュは可能性あり (QProcess の破棄時に finished が発行されるかどうかに依存する)
- 修正方針: トラックの clips を取得してから setClips で書き戻す (undo も取る)。connect はダイアログを開くたびではなく一度だけにする。実行中は generate を拒否する。生成結果を m_subtitleSegments に入れる。

### [Medium] タイムリマップ曲線の sourceFps が常に 30 で始まり、クリップ実 fps へのフォールバックが一度も効かない
- 区画: part11
- 場所: src/TimeRemap.h:56 (`double sourceFps = 30.0;`)、src/MainWindow.cpp:15111-15116 / 15130-15131 / 8665-8666 (`if (curve.sourceFps <= 0.0) curve.sourceFps = fps;`)、src/TimeRemapDialog.cpp:279-282 / 378
- 問題: 既定構築の TimeRemapCurve は sourceFps=30.0 なので、新規クリップでダイアログを開くと `entry.curve.sourceFps <= 0.0` が偽になり、probe した実 fps(24 / 25 / 60 など)が入らない。ダイアログの「Source FPS」も 30 で表示され、そのまま OK すると 30 fps の曲線がクリップ(ClipInfo::timeRemapCurve)に保存される。resolveFrame はソース時刻を 30 fps の格子でフレーム番号にし、decodeClipFrameByIndex(…, 30) で秒に戻してデコードする。
- 発生条件: 30 fps 以外の素材にタイムリマップを付け、ダイアログの fps を手で直さない。
- 影響: NearestFrame は実フレームでない 1/30 秒格子を拾うので、24 fps 素材ではスローモーションのコマが不均等に重複する。Blend / Optical Flow は 1/30 秒離れた 2 枚を混ぜるため、60 fps 素材では中間フレームを飛ばす。rotoSourceFps(TimelineFrameRenderer.cpp:1823-1826)はタイムリマップ付きクリップでこの 30 を優先するので、ロトのフレーム番号の基準もリマップを付けた時点で実 fps から 30 に変わる(ロトのずれは可能性あり)。
- 確度: 確認済み(既定値 30 → `<= 0` 判定が偽 → commitTimeRemapEntry でそのまま保存、までたどった)。ロトへの影響は可能性あり(ロトのキーの付け方までは追っていない)
- 修正方針: 既定値を 0(未設定)にして、MainWindow の既存のフォールバックを効かせる。旧プロジェクトの「30 だが未設定」の扱いは読み込み時に判断する。

### [Medium] setup.bat が Windows Defender に cmake.exe / cl.exe / link.exe のプロセス名除外を恒久的に追加する
- 区画: part11
- 場所: setup.bat:22-28
- 問題: `Add-MpPreference -ExclusionProcess 'cmake.exe'` など、フルパスを付けずにファイル名だけで除外する。Defender のプロセス除外は、その名前のプロセスが開くファイルをすべてスキャン対象から外す。ビルド後も解除しない。成否を見ずに必ず「[OK] Defender exclusions set」と表示する。
- 発生条件: README の手順どおり管理者権限で setup.bat を実行する。
- 影響: どこに置かれた `link.exe` / `cl.exe` という名前の実行ファイル(マルウェアの偽装も含む)でも、そのファイル操作が恒久的にスキャンされなくなる。C:\vcpkg とビルドディレクトリのパス除外も残る。権限が無くて失敗しても、利用者は設定されたと思い込む。
- 確度: 確認済み(スクリプトの記述そのもの)
- 修正方針: プロセス除外をやめ、必要ならビルドディレクトリのパス除外だけにする。実行前に同意を取り、結果を確認して表示する。解除方法も示す。

### [Medium] build.bat が Qt6 をインストールせず、setup.bat と別の vcpkg ルートを使うため、新しい環境では CMake configure が失敗する
- 区画: part11
- 場所: build.bat:84-110, 117 / CMakeLists.txt:20
- 問題: build.bat が vcpkg で入れるのは FFmpeg だけで、qtbase / qtmultimedia を入れない。vcpkg.json(manifest)もリポジトリに無い。CMakeLists は `find_package(Qt6 REQUIRED ...)` なので、Qt が別に入っていない環境では configure が失敗する。vcpkg ルートも setup.bat(C:\vcpkg)と違う(%USERPROFILE%\.veditor-vcpkg)。このため setup.bat で Qt を入れた環境でも、build.bat は Qt の無い新しい vcpkg を toolchain に指定し、同じ build\ のキャッシュを別の vcpkg で再構成する。
- 発生条件: 新しいマシンで build.bat を実行する。または setup.bat の後に build.bat を実行する。
- 影響: 「ワンクリックビルド」が Qt6 not found で止まる。または既存キャッシュと toolchain が食い違ったまま再構成される。
- 確度: 確認済み(build.bat の install 対象と CMake の REQUIRED を突き合わせた)。setup.bat 後の挙動は可能性あり(CMake キャッシュにある Qt6_DIR が残れば通る)
- 修正方針: vcpkg.json を置いて依存(qtbase, qtmultimedia, ffmpeg と features)を一か所で宣言し、両スクリプトから同じ vcpkg ルートで使う。

### [Medium] 逆再生クリップの端ドラッグのトリムが、逆の端の素材を削る
- 区画: part12
- 場所: src/Timeline.cpp:3394-3419 (tryHitTrimEdge), src/Timeline.cpp:3686-3704 (mouseMoveEvent の TrimLeft / TrimRight)
- 問題: GUI のトリムは、左端なら常に inPoint を、右端なら常に outPoint を動かす。`reversed` を見ていない。逆再生クリップは局所 0 秒に outPoint、末尾に inPoint を出す (ClipInfo::sourceSecondAtLocalTime:107-108、filmstrip も :2705 で反転して描く)。そのため左端を右へ寄せると、再生の末尾 (右側) の素材が消える。leadInSec が同じ量だけ増えて右端は固定されるので、残った区間のどの時刻でも表示されるフレームが変わる。右端のトリムは outPoint を削るので、再生の先頭 (左側) の素材が消え、左端の絵も変わる。
- 発生条件: 逆再生 (reversed=true) のクリップの左端または右端をドラッグしてトリムする。例: in=0 / out=10 のクリップの左端を 2 秒右へ寄せる。トリム前は t=2 に source 8 が出ていた。トリム後は t=2 に source 10、t=10 に source 2 が出る。期待は t=2..10 に source 8→0 (outPoint=8)。
- 影響: 削ったつもりの区間と違う区間が消え、残したつもりのカット点の絵もずれる。逆再生クリップでは、ドラッグ中のプレビューと編集の意図が合わない。
- 確度: 確認済み (sourceSecondAtLocalTime の反転式と、トリム時の inPoint / outPoint / leadInSec の更新を数値で追跡した)
- 修正方針: reversed のとき、左端のトリムは outPoint を `orig - delta*speed` で動かし、右端のトリムは inPoint を動かす。leadInSec の補正は左端のときだけ行う。trimops::applyTrim (TrimOps.cpp) も同じ規則に揃える。

### [Medium] 選択ベースの削除・分割・リップル削除・カラーマッチ LUT が、トラックのロックを無視する
- 区画: part12
- 場所: src/Timeline.cpp:6886-6900 (deleteSelectedClip の deleteFrom)、src/Timeline.cpp:6295-6299 (splitAtPlayhead の splitTrack)、src/Timeline.cpp:8665 (rippleDeleteSelectedClip が skipLocked の既定値 false で呼ぶ。Timeline.h:1737)、src/Timeline.cpp:937-949 (applyColorMatchLutToSelectedTimelineClip)
- 問題: これらの経路は `isLocked()` を確認しない。TimelineTrack::setLocked (Timeline.h:633) は選択を解除しない。そのため、選択してからロックしたクリップ (リンク選択で選ばれた相方を含む) が、Delete / 分割 (MainWindow::splitClip → splitAtPlayhead) / カラーマッチの LUT で書き換えられる。リップル削除は、ロック中のトラックにある範囲内のクリップも分割・削除・左詰めする。同じファイルの bladeAllTracksAtPlayhead:6215、closeAllGaps:8679/8698、duplicateSelectedClips:8708、MCP 系 (*ByIndex) はロックを確認しており、README:275 / :743 / :767 は「ロック中のトラックは編集しない」と書いている。
- 発生条件: クリップを選択 → そのトラックをロック → Delete / Ctrl+K 相当の分割 / リップル削除 / カラーマッチの適用。リップル削除は、ロック中のトラックが選択範囲と時間で重なるだけで起きる。
- 影響: ロックで保護したつもりのトラックが編集され、同期がずれる (undo では戻せる)。
- 確度: 確認済み (各関数に lock の分岐が無いこと、setLocked が選択を保持すること、呼び出し元の MainWindow.cpp:11422 / :11431 / :11440 と ColorMatchDialog.cpp:255 を確認した)
- 修正方針: deleteFrom / splitTrack / LUT の適用でロック中のトラックをスキップする (または拒否する)。rippleDeleteSelectedClip は skipLocked=true を渡す。または setLocked で選択を解除する。

### [Medium] グループ化した複数映像トラックのクリップを MCP `move_clip` / 再生ヘッドへ移動すると、全メンバーが 1 本のトラックへ集められる
- 区画: part12
- 場所: src/Timeline.cpp:7283-7333 (moveClipByIndex のメンバー収集と destinationTrack の決定)、src/Timeline.cpp:7447-7488 (insertAtRequestedTime)
- 問題: linkGroup が同じクリップはすべてメンバーになる。行き先は「選択クリップと同種なら destinationTrack、異種なら oppositeDestination」の 2 本しかない。setClipGrouping (:6835) は別々のトラック (V1 と V2 など) のクリップを同じ linkGroup にできるので、V2 のメンバーも V1 (選択側の行き先) へ挿入され、元の V2 から消える。開始時刻が同じなら、:7457 の境界挿入で 2 本が V1 上に直列に並び、選択クリップはメンバーの長さだけ右へ押される。時間が部分的に重なれば挿入に失敗して `moved:false` (重なりの reason) になり、moveSelectedClipToPlayhead は 64 回試したあと「リンクしたクリップを配置できる移動先がありません」で終わる。
- 発生条件: 別トラックの映像クリップ 2 個以上をグループ化 (setClipGrouping) し、そのうち 1 個を MCP `move_clip` またはエルゴ操作の「再生ヘッドへ移動」(moveSelectedClipToPlayhead:6099) で動かす。nudge (m_nudgeBatchActive) は元のトラックを保つので起きない。
- 影響: レイヤー構成が壊れる (上のトラックのクリップが V1 へ落ち、選択クリップの位置もずれる)。または移動自体ができない。
- 確度: 確認済み (メンバーの destinationTrack の代入 :7329-7330 と、pending plan の挿入順を追跡した)
- 修正方針: 同種のメンバーは「元のトラック + (newTrackIndex - trackIndex)」のトラックへ送る (トラック数が足りない・ロック中なら拒否する)。またはリンクの相方 (異種) 以外のメンバーは元のトラックのまま時間だけずらす。

### [Medium] 無音カットが、ロック・リンク相手・位置キーの sidecar を無視する
- 区画: part13
- 場所: src/Timeline.cpp:9860-9952 (applySilenceCutToClip)、呼び出しは 10855 と src/MainWindow.cpp:3670-3672
- 問題: (1) 分割したクリップを setClips で差し込むだけで、remapTrackMatteLocked / remapClipParentEntriesAfterMutation / roto の remap を呼ばない (同じ置き換えをする applyMusicRemix:10115-10130 は呼んでいる)。そのため、後ろのクリップの "t:i" キー (roto / マット / 親子) が (分割数 - 1) だけずれ、別のクリップに付く。(2) トラックのロックを確認しない (メニューも無効化しない)。(3) 分割片それぞれに allocateLinkGroup() で新しい linkGroup を振り、リンク相手 (A1 の音声など) は切らない。映像だけが詰まって短くなり、音声はそのまま残る。
- 発生条件: V1 クリップの右クリック、またはプレビューの右クリックから「無音を自動カット...」を実行する。後ろに roto / マット / 親子を設定したクリップがある、トラックがロック中、またはリンクした音声がある。
- 影響: roto やマットが別のクリップに掛かる (書き出しも同じ)。ロック中のトラックが編集される。カット以降の A/V 同期が崩れ、リンクも切れる。
- 確度: 確認済み
- 修正方針: 先頭でロックを確認し、snapshotTrackClips → 置き換え → remapCarriersAfterExternalMutation の順にする。リンク相手にも同じ keep 区間を適用してから linkGroup を再割り当てする (できない場合は拒否する)。

### [Medium] 速度・音量・パン・トランジションの GUI 操作が、V1 選択と同じ番号の A1 クリップを書き換え、ロックも無視する
- 区画: part13
- 統合メモ: part06 の同旨指摘(速度 / 持続時間ダイアログ・速度ランプ)を統合。
- 場所: src/Timeline.cpp:11374-11390 (setClipSpeed)、11392-11422 (setClipVolume / setClipPan)、11714-11789 (applyTransitionToSelected)、11839-11885 (clearTransitionsOnSelected)。呼び出しは src/MainWindow.cpp:11695-11750 とクリップのコンテキストメニュー
- 問題: どれも m_videoTrack->selectedClip() の番号 sel を、そのまま m_audioTrack (A1) の同じ番号に当てる。リンクグループは見ない。V1 / A1 のロックも確認しない。
- 発生条件: V1 と A1 のクリップ番号がずれている (片側だけ分割・削除・追加した、BGM が A1 にある、リンク音声が A2 にある)。または V1 / A1 がロック中。
- 影響: リンクしていない別の音声クリップの速度・音量・フェードが変わる。本来のリンク相手は変わらず、速度変更の後に A/V がずれる。ロック中のトラックが変更される。
- 確度: 確認済み
- 修正方針: リンク相手は linkGroup で探す (applyTrimLinked と同じやり方)。ロック中の相手がいれば操作全体を拒否する。

### [Medium] リンクしたクリップのドラッグで、ロック中のトラックにある相手も動く
- 区画: part13
- 場所: src/Timeline.cpp:13107-13135 (captureLinkedDragPartners)、13137-13167 (applyLinkedDragDelta)。クロストラック drop の 13436-13442 も同様 (先頭の相手 "anchor" はロックに関係なく移動する)
- 問題: リンクしたトリム (captureLinkedTrimPartners:13177-13203) は「ロック中の相手がいれば操作全体を拒否」しているが、リンクしたドラッグは相手のロックを確認せずに partners に入れ、applyDragMove (ロック確認なし、Timeline.cpp:2227) で leadInSec を書き換える。TimelineTrack 側のロック判定はドラッグを始めたトラック自身にしか効かない。
- 発生条件: ストーリーラインモード OFF で、ロックしていないトラックのクリップを同じトラック内でドラッグする。そのリンク相手 (例: 音声) がロック中のトラックにある。
- 影響: ロックしたトラックのクリップが動く。ロックを信頼して保護していた音声の配置が崩れる。
- 確度: 確認済み (handleBodyClick:3457 → linkedDragStarted → capture、mouseMove:3610 → applyLinkedDragDelta)
- 修正方針: captureLinkedDragPartners でロック中の相手を見つけたら、トリムと同じく partners を空にしてドラッグを拒否する。クロストラック drop の anchor にも同じ確認を入れる。

### [Medium] 右クリックメニューやダイアログを開いている間に MCP が編集すると、古い参照や番号でクリップを書き換える
- 区画: part13
- 場所: src/Timeline.cpp:10269 (const ClipInfo &aClip = track->clips()[clipIndex])、10335 / 10416-10467 / 10527-10541 (aMenu.exec と dialog.exec の後に aClip と clipIndex を使う)、10994-10996 (exec 後に範囲を再確認せず clips()[clipIndex].shapes.first())、12538-12721 (normalizeAudioClipPeak。12678 の processEvents の後に 12720 で clips[clipIdx] = clip と、古い ClipInfo 全体を書き戻す)
- 問題: MCP サーバ (McpHttpServer) は GUI スレッドの QTcpSocket::readyRead で動くので、QMenu::exec / QDialog::exec / processEvents の入れ子イベントループの間にも編集ツールが実行される。exec の後は track->clips() の中身と clipIndex が変わっている可能性があるのに、aClip は QVector の古いバッファへの参照のまま使われる (setClips で旧バッファが解放されると dangling)。clipIndex の範囲も確認しない。
- 発生条件: メニューやダイアログ (ダイアログレベラー、ミュージックリミックス、シェイプモディファイア) を開いている間、または 60 秒を超えるクリップのノーマライズ中に、AI エージェントが MCP でクリップを削除・挿入する。
- 影響: 解放済みメモリの読み出しによるクラッシュ、別のクリップへのエンベロープ・リミックスの適用、ノーマライズでの他の編集の巻き戻し、範囲外の operator[] (未定義動作)。
- 確度: 可能性あり (MCP が GUI スレッドのイベントで動き、モーダル中も処理されることは確認済み。MCP 側にモーダル中の受付を止める仕組みは見当たらない。実機では再現していない)
- 修正方針: exec の前に必要な値をコピーし、exec の後に uid でクリップを引き直して存在とロックを再確認する。ノーマライズは音量だけを、uid で引き直したクリップに書く。あるいはモーダル中は MCP の変更系ツールを拒否する。

### [Medium] Twitch のストリームキーが資格情報ボールトを通らず、平文で QSettings に保存・読込される
- 区画: part14
- 場所: src/TwitchStreamDialog.cpp:100, src/TwitchStreamDialog.cpp:127(関連: src/CredentialStore.cpp:26, src/CredentialDialog.cpp:295)
- 問題: `twitch/stream_key` は CredentialStore の「機密キー」一覧に入っており、Windows では CredentialVault(資格情報マネージャ)へ保存し QSettings 側の平文を消す設計になっている。ところが TwitchStreamDialog は `QSettings().setValue("twitch/stream_key", ...)` で直接平文を書き、読込も `QSettings().value(...)` だけで行う。チェックを外して「生成」しても、保存済みのキーは消えない。
- 発生条件: 「ストリームキーを保存する」にチェックを入れて「生成」を押す。または資格情報ダイアログで先にボールトへ保存してから Twitch ダイアログを開く。
- 影響: ストリームキー(配信乗っ取りに使える秘密)がレジストリ/ini に平文で残る。資格情報ダイアログの「ボールトへ移行」を行っても、このダイアログを使うたびに平文が戻る。逆に、ボールトに保存したキーはこのダイアログに出てこない。ボールトが QSettings より優先されるため、両方に別々の値が残り、どちらが使われるかがずれる。
- 確度: 確認済み(CredentialStore::get/set はボールト優先。TwitchStreamDialog は QSettings を直接使っていることを確認)
- 修正方針: 読込を `creds::CredentialStore::get(nullptr, "twitch/stream_key")`、保存を `CredentialStore::set` に置き換える。チェックが外れているときは `CredentialStore::clear` を呼ぶ。

### [Medium] GPU プレビューの FX パック(Vignette / Blur / Noise)が CPU 書き出しと別の式で、既定の Vignette はプレビューに一切出ない
- 区画: part14
- 場所: src/VideoEffect.cpp:2183-2208(applyVignette)、src/VideoEffect.cpp:1976-2023(applyBlur)、src/VideoEffect.cpp:2258-2272(applyNoise)。プレビュー側は src/GLPreview.cpp:689-704(fxBlur)、712-716(fxApplyNoise)、736-742(fxApplyVignette)、2383-2407、src/VideoPlayer.cpp:92-100
- 問題: GPU が使えるとき、エフェクトが Blur/Noise/Sepia/Grayscale/Invert/Vignette だけのクリップは、プレビューを GLPreview のシェーダで描く(`gpuPreviewSupportsEffect`)。一方、書き出し(TimelineFrameRenderer → `VideoEffectProcessor::applyEffect`)は CPU 実装を使う。両者の式が違う。
  - Vignette: GPU は `smoothstep(r, r+0.35, distance(uv,0.5))` で、uv 距離は最大でも約 0.707。CPU は `normDist = dist/maxDist`(角で 1.0)に対して `1 - I*t²`。既定の radius 0.8 では GPU 側は常に 0(効果なし)だが、CPU 側は角が最大 50% 暗くなる。
  - Blur: GPU は半径を 8 に丸めたうえ、ループが ±4 しかないので最大 4px の 1 パス box。r<0.5 なら効果なし。CPU は `r = max(1, radius)` の 3 パス box で、radius 0 でもぼける。radius 20 なら 20px×3 パス。
  - Noise: GPU は全チャンネル同じ値で振幅 ±amount/100×0.2(amount 20 なら約 ±10 階調)。CPU はチャンネルごとに独立で ±amount 階調(±20)。
- 発生条件: GPU プレビューが有効な状態で、クリップに Vignette(既定値のまま)/ Blur(radius ≥ 5 程度、または 0)/ Noise だけを付けて、プレビューと書き出しを比べる。
- 影響: プレビューでは変化がないのに、書き出すと周辺減光が入る。ぼかしの強さが数倍違う。TimelineFrameRenderer のコメントにある「プレビューとピクセル一致」が成り立っていない。
- 確度: 確認済み(シェーダと CPU の式、および GPU 経路を選ぶ条件 `stackRequiresClipLocalCpu` をたどった)
- 修正方針: どちらか一方の式に揃える(CPU の式をシェーダへ移植する)。揃えるまでは、この 3 種を `gpuPreviewSupportsEffect` から外し、CPU プレビューで描く。

### [Medium] 旧「Video Effects」ダイアログで Enabled を切り替えると、UI を持たないエフェクトのパラメータが隠れたスピンの値で上書きされる
- 区画: part14
- 場所: src/VideoEffectDialogs.cpp:383-398(onParamChanged)、src/VideoEffectDialogs.cpp:378-379(updateParamUI の default)、src/VideoEffectDialogs.cpp:133-134, 235(addEffect)
- 問題: `onParamChanged` は、どのエフェクトでも `param1/2/3 = m_param1Spin/2/3->value()` を無条件に書き込む。一方 `updateParamUI` が値を入れるのは 11 種だけで、それ以外(Levels、Glow、LensDistortion、WaveWarp、Echo など約 50 種)は default 分岐でスピンを隠すだけになり、前に表示したエフェクトの値か初期値 0(範囲 0..100)が残る。そのため Enabled チェックの切り替えや、Map source コンボの変更で、そのエフェクトの実パラメータが壊れる。さらに、追加用コンボには `allTypes()` の約 60 種が並ぶが、`addEffect` は 11 種以外を黙って return するので、ほとんどの種類は「Add」を押しても何も起きない。
- 発生条件: エフェクトパネルで Levels(0/255/1.0)などを付けたクリップを選び、メニューの「Video Effects」を開いて、そのエフェクトの Enabled を OFF→ON する。その後 OK。
- 影響: Levels は param1..3 = 0 になり(white<=black の補正で 2 階調化)、LensDistortion は k1/k2/scale が 0 になる(scale は 0.5 に丸められて拡大される)。このように、無効/有効を切り替えるだけで見た目が大きく変わる。OK を押すと `setClipEffects` で確定し、保存される。
- 確度: 確認済み(MainWindow::videoEffects → VideoEffectDialog → setClipEffects の流れをたどった)
- 修正方針: `onParamChanged` で、Enabled の変更時は enabled だけを書く。パラメータは、表示中のスピンに対応するものだけを書く。追加コンボは `addEffect` が対応する種類に限るか、`paramSchemaFor` から UI を作る。

### [Medium] VFX ジェネレータのプレビュー(360×220)と生成クリップ(プロジェクト解像度)で、ピクセル単位のパラメータの見た目が数倍違う
- 区画: part14
- 場所: src/VfxGeneratorDialog.cpp:604(`render(type, QSize(360, 220), ...)`)、src/VfxGenerators.cpp:686-695, 715(ShockWave の半径・速度・リング幅は px)、849-852(EnergyBeam の core/halo は px)、662-663(Lightning の coreWidth)、1084, 1147(EnergyShield の cellSize・rippleSpeed)、526-533(Explosion の gravity は px/s²)。生成側は src/MainWindow.cpp:14800, 14831
- 問題: 位置と規模は短辺比で正規化されているが、上に挙げた値は絶対ピクセルのままで、キャンバスの大きさで換算されない。ダイアログのプレビューは 360×220 で描くのに、確定したクリップは `m_projectConfig.width/height`(例 1920×1080)で描く。
- 発生条件: 衝撃波を既定値(speed 300px/s、1.5 秒)で作る。プレビューでは 1 秒足らずでリングが画面外へ出るが、1080p のクリップでは最終半径 458px(画面高の約 4 割)までしか広がらない。ビームのハロー 34px も、プレビューでは太く、1080p では細い。
- 影響: プレビューを見ながら調整した結果と、タイムラインに入るクリップの見た目が合わない(プレビューと出力の不一致)。
- 確度: 確認済み
- 修正方針: ピクセル単位の値を「基準短辺 1080px あたり」と定義し、描画時に `shortEdge(size)/1080` を掛ける。あるいは、プレビューをプロジェクトのアスペクトと解像度で描いてから縮小する。

### [Medium] Vimeo の access / refresh token が CredentialStore を通らず、資格情報の vault 移行後は Vimeo がログアウトする (token も平文で保存される)
- 区画: part15
- 場所: src/VimeoOAuth.cpp:81-84 (defaultConfig の読み込み)、:311-316 (applyTokenPayload の保存)。関連: src/CredentialStore.cpp:13-29 (vimeo_oauth/access_token と refresh_token を sensitive key に登録)、:146-166 (migrateSensitiveKeysToVault)、src/CredentialDialog.cpp:307
- 問題: CredentialStore は vimeo_oauth/access_token と vimeo_oauth/refresh_token を秘密情報として扱う。migrateSensitiveKeysToVault は値を vault に移して QSettings から消す。ところが VimeoOAuth は両方の token を QSettings に直接 setValue / value で読み書きしていて、vault を一度も見ない。
- 発生条件: (1) 認証情報ダイアログで vault への移行を実行すると、次回起動時の defaultConfig が空の token を読む。(2) 認証や refresh のたびに token が平文の QSettings (Windows ではレジストリ) に書かれる。
- 影響: (1) Vimeo の認証が黙って外れる。refresh_token も失うので auto-refresh もできない (vault に残った値は誰も読まない)。(2) client_secret は vault に入るのに、より強い権限を持つ token が平文のまま残る。
- 確度: 確認済み (grep で vimeo_oauth/*_token を参照しているのは VimeoOAuth.cpp の QSettings 直アクセスだけ)
- 修正方針: 読み込みを creds::CredentialStore::get(nullptr, key, QString(), false) に、保存を CredentialStore::set に置き換える。refresh_token が空の応答では CredentialStore::clear で消す。

### [Medium] Vimeo のダイアログで認証しても、アップロードは別の AuthClient の空 token を使って失敗する
- 区画: part15
- 場所: src/VimeoUploadDialog.cpp:30 (ダイアログ専用の AuthClient を new)、:245 (exchangeAuthorizationCode)。src/MainWindow.cpp:18136-18144 (Manager には m_vimeoOAuth を渡す)。src/VimeoUploadManager.cpp:121-131 (m_oauth->accessToken() / config())
- 問題: ダイアログは defaultConfig から自前の AuthClient を作り、Authenticate で受け取った token はその AuthClient だけに入る (と QSettings にも書かれる)。Manager が持つ MainWindow の m_vimeoOAuth は起動後に一度だけ defaultConfig を読んだ別インスタンスで、tokenReceived を受けて更新する経路がない。
- 発生条件: token を保存していない状態でダイアログを開き、Authenticate → コード貼り付けで認証に成功したあと Add Job する。
- 影響: Manager は空の token で UploadClient を作るので、startUpload がすぐに「Vimeo access token is empty」で失敗する。ダイアログは「Vimeo 認証が完了しました」と表示しているのに、アプリを再起動するまでアップロードできない。
- 確度: 確認済み (ダイアログの m_oauth と MainWindow::m_vimeoOAuth が別インスタンスで、間をつなぐ connect / setAccessToken が無い)
- 修正方針: Manager が持つ AuthClient をダイアログに渡して共有する (コンストラクタ引数にするか、manager->authClient() を公開する)。

### [Medium] V2 以上のオーバーレイのデコーダが stream_index=-1 でシークし、start_time を足していないので、start_time が 0 でない素材では違うフレームを出し、毎 tick シークし直す
- 区画: part15
- 場所: src/VideoPlayer.cpp:8398-8402 (runOverlayDecodeForDecoder の av_seek_frame(d->formatCtx, -1, targetUs, ...))、:7661-7665 (openTrackDecoder の clipIn シーク)。対照: :8256-8260 (decodePoolFrame は pts から start_time を引く)、:5713-5722 (V1 の streamTimestampForPosition は start_time を足す)
- 問題: stream_index に -1 を渡すと、av_seek_frame は timestamp を AV_TIME_BASE 単位の「絶対」時刻として扱う。一方、プールのデコーダは位置を「pts − start_time」で数えていて、targetUs もこの相対座標で計算している。相対の値をそのまま絶対時刻として渡すので、start_time の分だけ手前に着地する。追い付きのデコードは 4 フレーム (初回は 8 フレーム) で打ち切られるので目標に届かない。
- 発生条件: V2+ (または V1 がアクティブでないときの V1 のプール経路) に置いた、start_time が大きい素材 (AVCHD の .MTS / .M2TS、MPEG-TS、edit list で先頭がずれた MP4 など) を再生する。
- 影響: 再生中のオーバーレイが目標より最大で start_time 分だけ前のフレームを表示する。次の tick ではドリフトが 4 フレームを超えたと判定してまた同じ位置にシークするので、カクつきと高い CPU 負荷が続く。書き出し (renderFrameAt) とプレビューが食い違う。V1 の経路は正しい。
- 確度: 確認済み (座標系の食い違いはコードで確定。見た目の症状は FFmpeg の stream_index=-1 の仕様から導いた)
- 修正方針: V1 と同じく、videoStreamIndex を指定し、av_rescale_q で stream の time_base に変換して start_time を足した timestamp でシークする (2 か所)。

### [Medium] YouTube アップロードにサインインする経路がアプリになく、アップロードが必ず「no valid OAuth token」で失敗する
- 区画: part16
- 場所: src/MainWindow.cpp:18050-18063, src/YoutubeUploadManager.cpp:208-219, src/YoutubeOAuth.cpp:205 / :382
- 問題: `onYoutubeUpload()` は `AuthClient` を作るだけで、`launchAuthFlow()`(ブラウザ認可)も `loadToken()`(保存済み refresh_token の読み込み)も呼ばない。リポジトリ全体でこの 2 つを呼んでいるのは selftest だけ。そのため `m_token` は常に空で、`processNext()` の `refreshIfExpired()` は refresh_token が空なので false を返し、`currentToken().isValid()` が偽になって即 `failCurrent("no valid OAuth token")` になる。さらに `YoutubeOAuthConfig::defaultConfig()` は最初に開いたときに 1 回だけ読まれるため、後から認証情報ダイアログで Client ID を登録しても反映されない。
- 発生条件: GUI の「YouTube アップロード」でファイルを選んで Upload を押す(常に)。
- 影響: 機能が丸ごと使えない。ダイアログには「認証情報ダイアログに登録してください」と出るが、登録しても成功しない。
- 確度: 確認済み(`grep launchAuthFlow|loadToken` の呼び出し元は YoutubeOAuth.h のコメントと selftests/oauth_refresh_e2e_selftest.cpp だけ)。README:661 の「real-authentication harness … no-op stub for CI」は CI 用ハーネスの話なので、アプリの機能が動かないことの説明にはならないと判断した。
- 修正方針: ダイアログに「サインイン」ボタンを付け、`launchAuthFlow()` の URL を `QDesktopServices::openUrl` で開く。起動時か初回表示時に `loadToken()` を呼ぶ。config は addJob のたびに読み直す。

### [Medium] YouTube の refresh_token が Vault を通らずに QSettings へ base64(実質平文)で保存され、認証情報ダイアログを開くと Vault へ移されて以後読めなくなる
- 区画: part16
- 場所: src/YoutubeOAuth.cpp:370-391, src/CredentialStore.cpp:13-18 / :146-166, src/CredentialDialog.cpp:307
- 問題: `youtube_oauth/refresh_token` は `sensitiveKeyList()` にあり、Vault に入れる設計の鍵。なのに `AuthClient::saveToken()` は `QSettings::setValue` で base64 にして直接書き、`loadToken()` も `QSettings::value` で直接読む。認証情報ダイアログは開くたびに `migrateSensitiveKeysToVault()` を実行し、QSettings にある値(base64 文字列のまま)を Vault へ移して QSettings から消す。その後 `loadToken()` は QSettings しか見ないので空を読み、ログイン状態が消える。
- 発生条件: Windows(Vault 対応)で refresh_token を保存したあと、認証情報ダイアログを 1 回開く。Vault 非対応の環境では、トークンが常に QSettings に平文(base64)で残る。
- 影響: ログインが黙って失われる。長期有効の refresh_token が暗号化されずにレジストリや ini に残る。
- 確度: 確認済み
- 修正方針: saveToken/loadToken を `CredentialStore::set/get("youtube_oauth/refresh_token")` に置き換える(base64 はやめる)。既存の base64 値を移すときは decode してから Vault に入れる。

### [Medium] ビットレート指定の書き出しでも libx264 / libx265 / libsvtav1 に `crf` を必ず渡すため、指定したビットレートが無視されて CRF 23(AV1 は 30)で出力される
- 区画: part16
- 場所: src/libavcore/Encode.cpp:440-461, :614-617
- 問題: `codecOptionsFor()` は `rateControl == Bitrate` のときも、`crf` を `defaultCrf`(23 / 30)にして必ず追加する(`add("crf", quality)` は無条件)。FFmpeg の libx264 ラッパー(libx264.c)は `bit_rate` で ABR を設定したあと、`crf >= 0` なら `i_rc_method = X264_RC_CRF` で上書きする。libx265 も crf を優先する。その結果、`bit_rate` は効かない。h264_nvenc / hevc_nvenc も Bitrate モードで `rc=vbr` と `cq=23` が付き、品質目標の動作になる(こちらは可能性あり)。
- 発生条件: 既定の「ビットレート」モードで、H.264 / H.265 / AV1 をソフトウェアエンコーダで書き出す(Exporter.cpp:367、RenderQueue.cpp:1716、ProxyManager.cpp:161 の経路)。
- 影響: 出力サイズとビットレートがユーザー設定と無関係になる。SNS の上限を超えたり、低ビットレートのプリセットでも大きなファイルになったりする。プロキシの軽量化ビットレートも効かない。README:150 の「Rate control: bitrate」と矛盾する。
- 確度: 確認済み(こちらのコード経路を追跡済み。crf が bitrate を上書きする点は FFmpeg libx264.c / libx265.c の実装に基づく)。selftest export_pro G1 は「旧実装の値」としてこの組み合わせを固定している。
- 修正方針: Bitrate モードでは `crf` / `cq` を付けない(nvenc は `rc=vbr` と `b:v` だけにする)。export_pro G1 の期待値も直す。

### [Medium] `grabFrameAt` が stream の start_time を考えずに秒数を解釈するため、start_time が 0 でない素材でプレビューと違うフレームを返す
- 区画: part16
- 場所: src/libavcore/FrameGrab.cpp:146-152 / :176-180、比較対象 src/VideoPlayer.cpp:5714-5733
- 問題: VideoPlayer は位置を「start_time を引いた相対時刻」として扱う(`timestamp += stream->start_time` / `-= start_time`)。一方 grabFrameAt は `seconds` をそのまま stream の time_base に変換して seek し、フレームの pts も絶対値のまま `targetSeconds` と比べる。clip.inPoint などの相対時刻を渡す呼び出し元(MainWindow.cpp:8541-8545 の `decodeClipFrameAtSourceTime`、:13496-13501 の sourceFrameFetcher、ThumbnailCache.cpp:97)では、start_time の分だけ前のフレームを返す。start_time より小さい時刻では先頭フレームを返す。
- 発生条件: MPEG-TS / M2TS / AVCHD や一部の MOV など、start_time が 0 でない素材(数百 ms〜数十秒)。
- 影響: 再生ヘッドの位置から取ったフレーム(マスク / ロト / 各種ツールの参照フレーム)やサムネイルが、プレビューの表示とずれる。
- 確度: 確認済み
- 修正方針: seek 先に `stream->start_time` を足し、比較用の pts から引く(VideoPlayer と同じ規約にする)。

### [Medium] MediaDecoder などが、壊れたパケット 1 つで「ストリーム終端」とみなし、プロキシ生成は途中で切れたまま成功扱いになる
- 区画: part16
- 場所: src/libavcore/Decode.cpp:358-367 / :393-406 / :435-441、呼び出し元 src/ProxyManager.cpp:206-207。同じパターンが src/libavcore/AudioExtract.cpp:696-708 と src/libavcore/VideoFilterGraph.cpp:913-918 にもある
- 問題: `sendPacketToDecoder` は `avcodec_send_packet` が 0 以外を返すと失敗を返し、`nextFrame` はそれを受けて `ended = true` にする。`avcodec_receive_frame` が EAGAIN / EOF 以外を返した場合も同じ。そのため、普通に起こる `AVERROR_INVALIDDATA`(壊れたパケット 1 つ)で以降のフレームをすべて捨てる。nullptr は EOF と区別できず(ヘッダにも明記)、ProxyManager はループを抜けると finalize して成功を返す。AudioExtract(Whisper の書き起こし)と VideoFilterGraph(手ぶれ補正)は、同じ 1 パケットで処理全体を失敗にする。ffmpeg CLI はこの種のエラーを飛ばして続行する。
- 発生条件: 途中に破損パケットがある素材(録画の途切れ、ネットワーク越しにダウンロードしたファイルなど)。
- 影響: プロキシが途中までしか作られず、プレビューでその先が欠ける(書き出しは元素材を使うので、プレビューと書き出しが食い違う)。書き起こしや手ぶれ補正は全体が失敗する。
- 確度: 確認済み
- 修正方針: `AVERROR_INVALIDDATA` などの回復可能なエラーはそのパケットだけ捨てて続行する。致命的なエラーは EOF と区別できる状態(`lastError()`)で返し、ProxyManager はそれを見て失敗にする。

### [Medium] `FrameEncoder::finalize()` が `av_write_trailer` の失敗を無視して成功を返す(音声パススルーの書き込み失敗も無視する)
- 区画: part16
- 場所: src/libavcore/Encode.cpp:1729-1733, :1189-1194
- 問題: finalize は最後に `av_write_trailer(m_outFmt)` を呼ぶが、戻り値を見ずに `std::nullopt`(成功)を返す。avio_close のエラーも確認していない(releaseAll)。mp4 / mov では moov をこの trailer で書くので、ここで失敗するとファイルは再生できない。`muxAudioPassthroughPackets` も `av_interleaved_write_frame` の失敗を fprintf するだけで、処理を続けて成功扱いにする。
- 発生条件: 書き出しの終盤でディスクが一杯になる、ネットワークドライブが切れる、など。
- 影響: 再生できない(moov のない)ファイルや、音声のないファイルが「書き出し完了」として報告される。
- 確度: 確認済み
- 修正方針: trailer の rc と `avio_closep` の rc を確認してエラーを返す。パススルーの書き込み失敗も finalize のエラーとして返す。

### [Medium] selftest がユーザー本人の YouTube 認証情報を消したり上書きしたりする
- 区画: part16
- 場所: src/main.cpp:257-269 / :356-376(credential-store)、src/main.cpp:940-963 から src/YoutubeOAuth.cpp:351-352(oauth-mock-e2e)
- 問題: `runCredentialStoreSelftest` は「sensitive key」のテストに本物の鍵 `youtube_oauth/client_secret` を使い、cleanup で `CredentialStore::clear` する(Vault と QSettings の両方から消える)。organizationName / applicationName は本番と同じ "VSimpleEditor" / "V Simple Editor" なので、ユーザーが登録した本物の Client Secret が消える。`runOAuthMockE2eSelftest` は mock サーバとトークン交換し、`parseTokenJson` から `saveToken()` が呼ばれるため、本物の `youtube_oauth/refresh_token` と expiry をモックの値で上書きする。
- 発生条件: リリースビルドで `--selftest=all` / `--selftest=credential-store` / `VEDITOR_ALL_SELFTEST=1` などを実行する(README とヘルプで案内されている)。
- 影響: 登録済みの認証情報が黙って消える、またはモックの値に置き換わる。
- 確度: 確認済み
- 修正方針: selftest では専用の organizationName か `QSettings::setPath` で設定ファイルを分け、Vault の target にもテスト用のプレフィックスを付ける。鍵は `cred_test/...` のようなテスト用の名前を、sensitive の扱いになるようにテスト経路で登録して使う。

### [Medium] yt-dlp が無いか起動できないと、ダウンロードダイアログのボタンがすべて無効のまま戻らない
- 区画: part16
- 場所: src/YtdlpDownloader.cpp:77-89, src/YtdlpDownloadDialog.cpp:96-100 / :116-120
- 問題: `QProcess::start` が FailedToStart になると、QProcess は `errorOccurred` だけを出し、`finished` を出さない。YtdlpDownloader は `errorOccurred` に接続していないので、`finished` が一度も届かない。ダイアログはダウンロード開始時に「ダウンロード」と「閉じる」を無効にしているため、どちらも押せないまま残る(「キャンセル」も、`cancel()` が NotRunning で即 return するので何も起きない)。
- 発生条件: tools/yt-dlp.exe が無く、PATH にも yt-dlp.exe が無い(Linux / macOS では名前が "yt-dlp.exe" 固定なので常に該当する)。
- 影響: 「ダウンロード中...」の表示のまま止まり、ウィンドウの×で閉じるしかない。エラーの理由も表示されない。
- 確度: 確認済み
- 修正方針: `errorOccurred` を接続して `finished(false, …, errorString)` を出す。実行ファイル名を OS ごとに切り替える。

### [Medium] MCP の `set_transition`（kind=video）と `add_text_overlay` が V1 / A1 のトラックロックを無視する
- 区画: part17
- 場所:
  - src/mcp/McpEditorTools.cpp:5099-5166（video 分岐。ロック検査は audio 分岐の 5033 行にしかない）→ src/Timeline.cpp:11714-11791 `applyTransitionToSelected` / 11839- `clearTransitionsOnSelected`
  - src/mcp/McpEditorTools.cpp:5287-5299（`add_text_overlay`）→ src/Timeline.cpp:8999-9022 `addTextOverlayToVideoClipsInRange`
- 問題:
  - `set_transition` は audio のときだけ `target.track->isLocked()` を見ている。video のときは V1 のロックを確認せずに `applyTransitionToSelected` を呼ぶ。この関数は V1 の隣のクリップと、A1 の同じ index のクリップ（`m_audioTrack`）にもミラーで書き込むが、どちらにもロック検査がない。
  - `add_text_overlay` はハンドラにも `addTextOverlayToVideoClipsInRange` にもロック検査がない。V1 で区間が重なる全クリップの `textManager` に書き込む。
  - `set_track_locked` の説明文は「ロック中はクリップ編集を拒否する」としており、`set_clip_label` / `trim_clip` / `dynamic_zoom` / `set_clip_enabled` などはロックを拒否している。この 2 本だけ扱いが揃っていない。
- 発生条件: `set_track_locked {"kind":"video","trackIndex":0,"locked":true}`（または A1 をロック）の後に、`set_transition {"clipIndex":0,"type":"CrossDissolve"}` か `add_text_overlay {"text":"x","startSec":0,"endSec":1}` を呼ぶ。
- 影響: ロックで守っているはずの V1 / A1 のトランジションとテロップが変わり、書き出しにも乗る。undo で戻すことはできる。
- 確度: 確認済み（ハンドラ → Timeline 側の関数まで読み、どこにも `isLocked` がないことを確かめた）
- 修正方針: video 分岐で V1 のロックを拒否する。ミラー先の A1 がロック中の場合も拒否する（または A1 へのミラーを止めて warning を返す）。`addTextOverlayToVideoClipsInRange` の前に V1 のロックを検査する。

### [Medium] `export_video` が jobId を返してからジョブが Rendering になるまでの間、変更系ツールが通ってしまい、書き出しの映像と音声が別の状態になる
- 区画: part17
- 場所:
  - src/mcp/McpEditorTools.cpp:1314-1328（`guardedWrite` は `RenderJobStatus::Rendering` のジョブしか見ない）
  - src/mcp/McpEditorTools.cpp:2985-3036（`job.timeline = timeline()` はライブの Timeline。autoLoudness のときは `loudnessauto::prepareJobAsync` が別スレッドで動き、`addJob` されるのはその完了後）
  - src/RenderQueue.cpp:1242-1264（Pending のジョブは、開始した時点のライブ Timeline を描画する）
- 問題:
  - ジョブが queue に入る前（autoLoudness の非同期準備中）と、Pending の間（GUI でキューが一時停止しているときなど）は、ガードが何も拒否しない。
  - 音声は、準備を始めた時点の計画スナップショットから作ったミックス（autoLoudness OFF のときは singleShot 時点のミックス）。映像は、Rendering が始まった時点のライブ Timeline から描かれる。
  - コード内のコメント（2995-2996 行）もこの差を認めているが、README には制限として書かれておらず、対策もない。
- 発生条件: `export_video {"outputPath":..., "autoLoudness":true}` で jobId を受け取り、`get_export_status` が `queued` のうちに `delete_clip` / `move_clip` / `open_project` などを呼ぶ。測定は長い素材だと数秒から数十秒かかる。
- 影響: 書き出したファイルで映像と音声の尺や内容がずれる（`open_project` の場合は、別のプロジェクトの映像に元のプロジェクトの音声が付く）。ジョブは成功扱いになるため、エージェントは気づけない。
- 確度: 確認済み（ガードの条件、prepareJobAsync から addJob までの経路、startNextJob がライブ Timeline を使うことを追跡した）
- 修正方針: MCP が投入して観測中のジョブ（観測テーブルの状態が queued / running）があるか、queue に Pending / Rendering のジョブがある間は、`toolMutatesTimeline` のツールを拒否する。あるいはジョブ投入の時点で Timeline のスナップショットを渡す。

### [Medium] VFX コントロール (グロー / ブルーム / 色収差 / ライトラップ) はプロジェクトに保存されるのに、書き出しには一切掛からない
- 区画: part19
- 場所: src/MainWindow.cpp:7266-7297 (パネルのシグナル → VideoPlayer::setPreview*)、src/MainWindow.cpp:9170-9218 (populate / applyVfxProjectState)、src/VideoPlayer.cpp:4753-4775
- 問題: パネルのシグナルは `VideoPlayer::setPreviewGlow/Bloom/ChromaticAberration/LightWrap` に渡るだけで、この関数は GLPreview の uniform を設定するだけです。値は `ProjectData::vfxState` としてプロジェクトに保存・復元されます。しかし `vfxState` を読むのは MainWindow と ProjectFile だけで、renderFrameAt / TimelineFrameRenderer / Exporter / RenderQueue には渡りません (GLPreview の setGlow などを呼ぶのも VideoPlayer だけ)。README にも「プレビュー専用」とは書かれていません。part02 にあるカラーパネルのビネットなどの指摘とは別のパネルです。
- 発生条件: 表示 → VFX コントロールでグローなどを有効にし、書き出す。
- 影響: プレビューでは光って見え、保存して開き直しても再現されるので掛かっていると思い込みやすいのに、書き出した動画には効果がありません。値はクリップ単位ではなくグローバルなので、全クリップのプレビューに掛かります。パネルの操作は undo 点も作らず、変更済み (dirty) フラグも立てません。
- 確度: 確認済み (vfxState と setGlow/Bloom などの参照先を全部 grep し、書き出し経路から参照されないことを確認)
- 修正方針: 書き出しのフレーム合成で同じ処理を掛ける (renderFrameAt に ProjectVfxState を渡す)。対応するまでは、パネルとヘルプに「プレビュー専用・書き出し非反映」と表示する。

### [Low] 複数フレームの WebP を、同じ QImageWriter への連続 write で書いて成功扱いにしている
- 区画: part01
- 場所: src/AnimatedExport.cpp:467-479
- 問題: Qt の webp プラグインはアニメーションの書き込みに対応していません。同じ QImageWriter(開いたままの QFile)に write を繰り返すと、RIFF/WEBP コンテナが連結されたファイルになり、各 write は true を返します。その結果、「1 フレーム目だけ書く」フォールバックに入らず、`exportFrames` は成功を返します。
- 発生条件: Animated Export で WebP(既定の形式)を選び、2 枚以上で書き出す。
- 影響: 「Exported N frames」と表示されますが、ビューアには 1 枚目だけが表示されます。ファイルサイズは N 倍の不正なファイルになります。
- 確度: 可能性あり(qwebphandler の write が毎回コンテナ全体を device に書き、supportsOption(Animation) が読み込み専用であることは記憶に基づく。このコードが 2 回目以降の write の戻り値だけで判定していることは確認済み)
- 修正方針: `writer.supportsOption(QImageIOHandler::Animation)` で判定する。対応していなければ最初から 1 枚だけ書き、警告を返す。または libwebp の anim encoder を使う。

### [Low] GIF のフレーム遅延を 100/fps の切り捨てで出すため、再生が速くなる(51fps 以上は 1cs になる)
- 区画: part01
- 場所: src/AnimatedExport.cpp:386-387、src/AnimatedExportDialog.cpp:79(fps は 1..60)
- 問題: 15fps は 6cs(16.7fps 相当、11% 速い)、24fps は 4cs(25fps)、30fps は 3cs(33fps)になります。誤差を次のフレームへ繰り越す処理がありません。51〜60fps は 1cs になり、主要ブラウザは 0〜1cs を 10cs として扱うため、10fps でゆっくり再生されます。
- 発生条件: GIF を書き出す(既定の 15fps でも起こります)。
- 影響: GIF の再生時間と速度が、元の連番と合いません。
- 確度: 切り捨ては確認済み。ブラウザが 1cs を 10cs に補正する点は、既知のブラウザの挙動に基づく可能性ありです。
- 修正方針: 累積時刻から各フレームの遅延を求めて誤差を分散させる。fps は 50 を上限にする(または 2cs 未満にしない)。

### [Low] SVG を取り込むと 240×240 に引き伸ばされる
- 区画: part01
- 場所: src/AffinityVectorImporter.cpp:61-76、呼び出し元 src/ImportHubDialog.cpp:360 と :395
- 問題: `loadSvg(path, QSize(240,240))` で描いた画像を、そのまま `imageImportRequested` に渡しています。`QSvgRenderer::render(&painter)` は既定でアスペクト比を保たず、デバイス全体に描きます。
- 発生条件: Import Hub で横長または縦長の SVG を取り込む。
- 影響: 取り込んだ画像の縦横比が崩れ、240px の低解像度になります(プレビュー用のサイズが、そのまま素材として使われます)。
- 確度: 確認済み
- 修正方針: プレビューと取り込みでサイズを分ける。取り込みは defaultSize(必要なら拡大)で描き、`setAspectRatioMode(Qt::KeepAspectRatio)` を使う。

### [Low] Codex に MCP のトークンをコマンドライン引数で渡している。パスに ' が入ると起動に失敗する
- 区画: part01
- 場所: src/AiChatDock.cpp:290-298
- 問題:
  - `-c mcp_servers.veditor.env={VEDITOR_MCP_TOKEN='<token>'}` が argv に載るため、同じマシンの他のユーザーやプロセスが ps などで読めます(Claude 経路は 0600 の一時ファイルに置いています)。
  - 実行ファイルのパスを TOML のシングルクォート文字列に埋め込んでいますが、`'` をエスケープしていません。そのため、`C:/Users/O'Neil/...` のようなパスでは設定の解析に失敗します。
- 発生条件: Codex プロバイダーを使う(マルチユーザーの環境では、トークンが漏れる)。
- 影響: MCP サーバのトークンが漏れると、同じホストの他のユーザーがエディタを操作できます。
- 確度: 確認済み(トークンが argv に載ることと、エスケープがないこと)
- 修正方針: トークンは環境変数の継承(QProcessEnvironment に VEDITOR_MCP_TOKEN を設定し、Codex の env_vars で引き継ぐ)か、設定ファイル経由で渡す。TOML の文字列はリテラルのクォートをエスケープするか、パスを " 形式で組み立てる。

### [Low] メーターの「ノーマライズ」が減衰するピークホールドの値を使う(+3dB 上限)
- 区画: part01
- 場所: src/AudioMeterWidget.cpp:349-358、:273-280、:243-246
- 問題: ゲインを `currentPeakHoldDb()` から求めていますが、この値は 1.5 秒後から 50ms ごとに 1.7dB 減衰し、+3dB で頭打ちになります。停止中もタイマーで減衰し続け、-60dB まで下がると 0dB のゲインを送ります(何も起きず、通知もありません)。
- 発生条件: 再生を止めて数秒後にノーマライズを選ぶ。または、ピークが直近の 1.5 秒より前にあった。
- 影響: ノーマライズのゲインが、トラック全体のピークではなく直近の表示値で決まり、過大または無効になります。
- 確度: 確認済み
- 修正方針: トラック全体を解析して(ラウドネス解析と同じ経路で)ピークを求める。少なくとも、ホールド値がないときはユーザーに通知する。

### [Low] AudioEQProcessor のワーカースレッドが this を生のまま捕捉し、破棄を待たない
- 区画: part01
- 場所: src/AudioEQ.cpp:462-510(QThread::create([this,...]))、:145-149
- 問題: ワーカーは `runFFmpeg` / `parseProgress`(m_totalDuration)/ emit で this を使いますが、AudioEQProcessor の破棄時に interrupt も wait もしません。AIHighlight にはこの対策(trackAIHighlightThread)がありますが、こちらにはありません。
- 発生条件: Audio Equalizer / Audio Effects の処理中に MainWindow を閉じる(m_audioEQ は MainWindow が親)。
- 影響: 終了時に解放済みメモリへアクセスし、クラッシュすることがあります。
- 確度: 可能性あり(実際にクラッシュするかは、終了までの時間と ffmpeg の実行時間による。待たない構造であることは確認済み)
- 修正方針: QPointer と cancel フラグを使い、デストラクタで cancelled=true にしてスレッドを wait する。または AIHighlight と同じ登録方式にする。

### [Low] バッチ書き出しで実行中のタスクの行を「Remove」すると、表から消えるのに書き出しは続く
- 区画: part02
- 場所: src/BatchExportDialog.cpp:336-350、src/BatchExportQueue.cpp:228-238
- 問題: `Queue::removeTask` は Running のタスクを黙って無視しますが、ダイアログは戻り値を見ずに `removeRow` します。
- 発生条件: 書き出し中の行を選んで Remove を押す。
- 影響: 表から消えた後もレンダーが続き、完了 / 失敗の通知は rowForId が -1 になるため表示されません。ユーザーは止めたつもりになります。
- 確度: 確認済み
- 修正方針: removeTask に成否を返させ、失敗なら行を残してメッセージを出す (または実行中のタスクのキャンセルを実装する)。

### [Low] 自動保存をセッション中に OFF→ON すると、そのセッション自身の自動保存の「復元しますか？」が出る
- 区画: part02
- 場所: src/AutoSave.cpp:71-91, 193-200、src/MainWindow.cpp:7979-7983, 2685
- 問題: `start()` は起動時にクリーン終了マーカーを消します。再度 ON にしたときの `cleanShutdownBeforeAutoSaveStart = wasCleanShutdown()` は false になり、`checkForRecovery` が同じセッション内で作った自動保存ファイルを「復元候補」として emit します。
- 発生条件: 自動保存 ON で起動し、メニューで OFF→ON する。
- 影響: 起動直後でもクラッシュ後でもないのに、復元のダイアログが出ます。「復元」を選ぶと、今の編集より古い自動保存で上書きされるおそれがあります。
- 確度: 確認済み (start → marker 削除 → 再 start 時の property=false → handler の早期 return を通らない)
- 修正方針: 復元の確認はプロセスごとに 1 回だけにする (起動時の判定をメンバーに保持し、再 start ではチェックしない)。

### [Low] ハイライトから自動カットで、アスペクト (縦型 9:16 など) を選んでも何も起きず、追加は undo もロックも通らない
- 区画: part02
- 場所: src/AutoClipGenerator.cpp:144-165 (toTimelineClips)、72、src/MainWindow.cpp:17668-17680
- 問題: planClips は `hasCrop` を立てますが、`toTimelineClips` はクロップ (computeCropRect) を ClipInfo に反映しません。computeCropRect はどこからも呼ばれません。MainWindow は `TimelineTrack::addClip` で V1 に直接追加しており、undo への記録もトラックのロックの確認もありません。
- 発生条件: 自動カットダイアログでアスペクトを指定して計算し、Yes を押す。
- 影響: ダイアログで選んだアスペクトが無視されます。追加したクリップは Ctrl+Z で取り消せず、ロック中の V1 にも追加されます。
- 確度: 確認済み
- 修正方針: クロップを ClipInfo の crop / transform に写す (または UI からアスペクトを外す)。追加は Timeline の undo 付き API 経由にし、ロックを確認する。

### [Low] シーン検出の時刻が素材の start_time と再生位置を考慮せず、マーカーがずれる。sceneCheckInterval=0 では 0 除算になる
- 区画: part02
- 場所: src/AutoEdit.cpp:198, 217、呼び出し側 src/MainWindow.cpp:21194-21200
- 問題: `frame->pts * time_base` をそのまま秒にしています。start_time (MPEG-TS や一部の MOV では 0 でない) を引いておらず、pts が AV_NOPTS_VALUE の場合も考慮していません。MainWindow はこれをタイムライン時刻としてマーカーを置き、クリップの timelineStart / inPoint / speed を足していません。`frameCount % config.sceneCheckInterval` は 0 を検査していません。
- 発生条件: start_time が 0 でない素材、またはタイムラインの 0 以外の位置 / トリム済みのクリップでシーン検出する。
- 影響: マーカーが実際のカット位置からずれます (AV_NOPTS_VALUE なら巨大な負の時刻になります)。間隔 0 の設定を渡すと SIGFPE になります (現行の呼び出し元は既定値の 5 のみ)。
- 確度: 可能性あり (start_time の非 0 は素材に依存する。マーカーの座標の取り違えはコード上で確認済み)
- 修正方針: best_effort_timestamp − stream->start_time を使う。呼び出し側でソース秒からタイムライン秒へ写す。interval は 1 以上にクランプする。

### [Low] クロマキー精緻化ダイアログ: despill が緑でない画素に緑を足し、暗部を抜き、入力のアルファを捨てる
- 区画: part02
- 場所: src/ChromaKeyRefine.cpp:88-122 (despill)、52-70 (hsvDistance)、134 (QColor(srcLine[x]))
- 問題: despill は「平均へ寄せる」処理を常に行います。そのため g < (r+b)/2 の画素では緑が増えます (例: 純赤 (1,0,0) は spill=0.5 で (1,0.25,0) のオレンジになります)。hsvDistance は明度を無視するので、ノイズでわずかに緑寄りの黒 (例: (0,3,1) は h≈140°、s=1) がキー色と同一とみなされて透明になります。`QColor(QRgb)` はアルファを無視するので、透過 PNG の透明部が不透明になります。
- 発生条件: 赤・マゼンタ系や暗部を含む素材、または透過 PNG を読み込む。
- 影響: プレビューの結果が誤ります (このダイアログの結果は保存されないため、影響は表示に限られます)。
- 確度: 確認済み (数値で追跡)
- 修正方針: despill は g > avg のときだけ g = avg に寄せる (min を取る)。距離に明度 / 彩度の重みを入れる。元のアルファと掛け合わせる。

### [Low] glTF の外部バッファ URI をパーセントデコードせず、byteStride も無視する
- 区画: part02
- 場所: src/BlenderMeshImporter.cpp:374-377, 209-256
- 問題: glTF 2.0 の uri は URI 参照ですが、`baseDir + '/' + uri` をそのまま開いています。そのため "%E3%83%A1%E3%83%83%E3%82%B7%E3%83%A5.bin" や "my%20mesh.bin" のような名前を開けません。readAccessorBytes は bufferView.byteStride を読まず、インターリーブされた頂点を連続配列として読みます。
- 発生条件: 日本語名や空白を含む名前の .gltf + .bin (Blender などのエクスポーターは URI をエンコードする) を取り込む。インターリーブ形式の glTF を取り込む。
- 影響: メッシュが空になるか、崩れた形状になります。
- 確度: 可能性あり (エクスポーターがエンコードするかどうかに依存する。stride の無視はコード上で確認済み)
- 修正方針: `QUrl::fromPercentEncoding` でデコードする (data: 以外)。byteStride を考慮して要素ごとに読む。

### [Low] クラウドレンダー: ポーリングが 1 回失敗しただけで永久に Failed になり、API キーを平文 http やプレーンな QSettings に流す
- 区画: part02
- 場所: src/CloudRenderClient.cpp:534-544, 315-336、src/CloudRenderDialog.cpp:175-186, 417-440, 465-469
- 問題: (1) poll のネットワークエラーや 5xx でも `jobFailed` を出し、ダイアログはその状態を Failed にしてポーリングを止めます。cancel の通信失敗も Failed になります。(2) `QUrl::fromUserInput` は "render.example.com" を http:// に変えます。その上で `Authorization: Bearer` と `X-Api-Key` を平文で送ります。保存した API キーはエンドポイントに紐付かず、平文で QSettings に入ります。(3) submit の応答を読まず、サーバーが割り当てるジョブ ID を使いません (ローカルの UUID で status/<uuid> を問い合わせます)。
- 発生条件: 一時的な通信断。スキーム無しのエンドポイント入力。ジョブ ID をサーバー側で採番するプロバイダー。
- 影響: クラウドでは動いているジョブが「失敗」と表示され、結果の URL を受け取れません。トークンが平文で漏れます。採番型のプロバイダーでは、ポーリングが 404 で即 Failed になります。
- 確度: 可能性あり ((3) はプロバイダーの API 仕様に依存する。(1)(2) はコード上で確認済み)
- 修正方針: poll の失敗はリトライして、一定回数で Failed にする。https 以外ではキーを送らない (警告を出す)。キーは OS のキーチェーンに保存し、エンドポイントと組で保存する。submit 応答の jobId を採用する。

### [Low] ブラシアニメの文字単位モードが UTF-16 単位で分割し、サロゲートペアや結合文字を壊す
- 区画: part02
- 場所: src/BrushAnimation.cpp:401-415
- 問題: `m_text.mid(i, 1)` で 1 コードユニットずつ addText しています。絵文字や「𠮷」などのサロゲートペアは半分ずつになり、結合文字 (濁点 U+3099 など) は単独で描かれます。
- 発生条件: PerCharacter モードで、絵文字、拡張漢字、NFD の仮名を含む文字列を使う。
- 影響: その文字が豆腐になるか消えます。基底文字と結合文字が別の位置に描かれます。
- 確度: 確認済み
- 修正方針: QTextBoundaryFinder(Grapheme) で書記素単位に分割する。

### [Low] Camera3D::fromJson が未知のプロパティ名のトラックで PositionX を上書きする
- 区画: part02
- 場所: src/Camera3D.cpp:175-186, 999-1013
- 問題: `propertyFromName` は未知の名前で PositionX を返します。fromJson はそのまま `m_tracks[PositionX] = KeyframeTrack(未知名, …)` に置き換え、未知のトラックのキーを流し込みます。
- 発生条件: 新しいバージョンで追加されたカメラのトラックや、手で編集したプロジェクトを読み込む。
- 影響: カメラの位置 X のアニメが、別のプロパティの値で壊れます。さらにトラック名が未知名のまま保存されるため、次の読み込みで本来の PositionX が失われます。
- 確度: 確認済み
- 修正方針: 未知の名前はスキップする (propertyFromName が optional を返すようにする)。

### [Low] presets.json が読めない／壊れていると、effect-presets フォルダのユーザープリセットも全く読み込まれない
- 区画: part03
- 場所: src/EffectPreset.cpp:618-627（早期 return）, 604-616（非アトミック書き込み）
- 問題: `loadLibrary` は presets.json の open 失敗・JSON パース失敗で `return false` し、その後の effect-presets ディレクトリ走査に到達しない。`saveLibrary` は QFile 直書きで、書き込み途中の失敗や容量不足で壊れたファイルが残り得る（書き込み結果も未検査）。
- 発生条件: presets.json が途中で切れる／手で壊れる／権限で読めない。
- 影響: 個別ファイルとして健全に保存されているユーザープリセットまで一覧から消える。
- 確度: 確認済み
- 修正方針: presets.json の失敗はログに留めてディレクトリ走査を続ける。saveLibrary を QSaveFile にし write/commit の結果を返す。

### [Low] フリッカー除去の手動解析領域（px）が、解析（プロキシ）と適用（原本）で同じ数値のまま使われる
- 区画: part03
- 場所: src/DeflickerDialog.cpp:437-449, 401-405, 632-634, 732-734 / src/MainWindow.cpp:13493-13502, 8540-8544
- 問題: マスク使用時は `scaleAnalysisRegion` でキャンバス→フレーム座標へ変換するが、手動の X/Y/幅/高さは frameSize を無視して生の px で返す。解析は `frameFetcher`（ProxyManager のプロキシ）、適用は `sourceFrameFetcher`（原本）で解像度が違う。さらに setContext で手動欄にマスク矩形（キャンバス座標）をそのまま入れる。
- 発生条件: プロキシ有効のクリップで手動領域を指定して解析→適用。またはキャンバスと素材の解像度が違うクリップでマスク矩形が入った手動欄のまま実行。
- 影響: グラフで確認した補正（解析領域）と、実際に書き出す PNG の補正領域が別の場所になる。
- 確度: 確認済み
- 修正方針: 手動領域もキャンバス座標として保持し、両経路で `scaleAnalysisRegion` を通す（または解析も原本で行う）。

### [Low] コメントパネルの「現在時刻にコメント追加」とコメント選択でのシークが動かない
- 区画: part03
- 場所: src/CommentsDockWidget.cpp:170-174, 199-204 / src/MainWindow.cpp:18081, 18243
- 問題: `addCommentRequested(0)`（時刻は常に 0 のスタブ）と `commentSelected(tc)` を MainWindow がどこにも connect していない。`m_timecodeLabel` も更新されない。
- 発生条件: コメントパネルでボタンを押す／コメントをクリックする。
- 影響: コメントを追加できず、クリックしてもその時刻へ移動しない（返信と Frame.io 取り込みだけが動く）。
- 確度: 確認済み（grep で connect 無し）
- 修正方針: MainWindow で addCommentRequested を受けて再生ヘッド時刻で CommentTrack に追加、commentSelected でシークする。

### [Low] 「プラグインエフェクト」メニューは何も適用しないのに「Applied plugin」と表示する
- 区画: part03
- 場所: src/MainWindow.cpp:12688-12702（EffectPlugin の唯一の GUI 消費者）
- 問題: 選択したプラグインを見つけてもステータスバーに表示するだけで、クリップにも ClipInfo::effects にも何も書かない。README の機能一覧には「Plugin system (5 built-in)」とある。
- 発生条件: クリップを選択して Plugin Effects ダイアログで OK。
- 影響: 適用されたと誤認する。
- 確度: 確認済み
- 修正方針: エフェクトライブラリと同じ経路（applyEffectLibraryEntry）へ回すか、メニューを外す。

### [Low] イコライザーパネルで 1 バンドを触ると、他バンドの値が UI の刻みで上書きされ、無効バンドも有効に戻る
- 区画: part03
- 場所: src/EqualizerPanel.cpp:218-251, 270-287
- 問題: `onAnyControlChanged` は 4 バンド全部をウィジェットから読み直して emit する。gain は整数 dB、freq/Q はダイアル 1000 段に量子化され、`readBandFromWidgets` は `enabled = true` 固定、シェルフの Q は 0.7 固定。setEqSettings 時にもキャッシュがこの値で置き換わる。
- 発生条件: プロジェクトに gain 2.5 dB や enabled=false のバンド（AudioTrackFx.cpp:292-313 で読み込まれる）があるトラックで、別のバンドを 1 回動かす。
- 影響: 触っていないバンドの値が変わり、無効化していたバンドが有効化される。
- 確度: 確認済み
- 修正方針: 変更されたコントロールのバンドだけ更新し、他バンドは m_perTrackSettings の元値（enabled 含む）を保つ。

### [Low] 認証情報のマスク表示が 8〜11 文字の値をほぼ／完全に表示する
- 区画: part03
- 場所: src/CredentialStore.cpp:135-144 / 使用: src/CredentialDialog.cpp:41-53（ラベルはマウス選択可）
- 問題: 8 文字以上なら先頭 4 + 末尾 4 文字を表示するため、ちょうど 8 文字の値は全文、9〜11 文字もほとんど見える。
- 発生条件: 短い IG User ID / トークン / stream key を保存して配信認証情報ダイアログを開く。
- 影響: 画面共有・スクリーンショットで秘密値が漏れる。
- 確度: 確認済み
- 修正方針: 表示する文字数を長さに比例させる（例: 16 文字未満は全マスク、それ以上も先頭/末尾 2〜4 文字まで）。

### [Low] EXR 連番の alpha を非乗算(straight)のまま書く
- 区画: part04
- 場所: src/ExrSequenceWriter.cpp:294-308(`convertToFormat(Format_RGBA8888)` で非乗算にしてから、RGB と A を別々に書く)
- 問題: OpenEXR の規約では、色は alpha で乗算済み(associated alpha)です。ここでは乗算済みの ARGB32_Premultiplied 入力も非乗算に戻してから書くため、半透明の縁の色が規約より明るく書かれます。
- 発生条件: 連番を EXR で書き出し、alpha を保持する(keepAlpha)。
- 影響: Nuke や After Effects などで EXR を合成すると、半透明の縁に明るいふち(フリンジ)が出ます。
- 確度: 確認済み(変換の順序を確かめました。影響は EXR の規約からの推論です)
- 修正方針: keepAlpha のときは `Format_RGBA8888_Premultiplied` から RGB を取る(linearize は乗算を外して線形化し、乗算し直す)。

### [Low] `Expression::validate` が、ダミーの文脈(time=0, duration=0, value=0)で式を評価するため、正しい式を「Division by zero」として不正と表示する
- 区画: part04
- 場所: src/Expression.cpp:719-736(`validate` が `parser.validateOnly` で実際に評価する)、:265-274(0 による割り算と剰余を失敗にする)、利用側 src/ExpressionBindingDialog.cpp:357-364、src/MainWindow.cpp:16737-16741
- 問題: 構文の検査のはずが、値を評価します。`100 / time`、`value / duration`、`time % duration` など、実行時には正しく動く式が、検査では失敗します。
- 発生条件: 式バインディングのダイアログ、または「Expression」の入力で、time / duration / value で割る式を入れる。
- 影響: 構文のラベルが赤くエラーを表示します。MainWindow::editExpressions では、エラーとして拒否します。今後 MCP に validate を入れる場合も、同じ式を拒否します。
- 確度: 確認済み
- 修正方針: 検査は構文だけ(評価しない AST 構築)にするか、検査時の割り算と剰余の 0 チェックを外す。

### [Low] 式の関数で double → int の変換と std::clamp(lo > hi)の未定義動作
- 区画: part04
- 場所: src/Expression.cpp:403(`clamp` に lo > hi を渡せる)、:453 / :461(`static_cast<int>(round(NaN や巨大値))`)、:468-471(`seedRandom` の uint64 変換)、:595 / :605-606(`noise` / `noise2` の `static_cast<int>(floor(t))`)、:661-662(loopIn / loopOut)
- 問題: 式は MCP の `set_clip_expression` やプロジェクトファイルから自由に入ります。`clamp(x, 1, 0)`、`noise(time*1e12)`、`fbm(t, 1/0)` 相当(NaN)、`seedRandom(1e30)` は C++ では未定義動作です。
- 発生条件: 上のような引数の式を設定する。
- 影響: 通常のビルドでは不定の値で済みますが、`_GLIBCXX_ASSERTIONS` や UBSan のビルドでは abort します。プラットフォームによって結果が違います。
- 確度: 可能性あり(未定義動作であることは確認済みです。リリースビルドで実害があるかどうかは、コンパイラ次第です)
- 修正方針: 変換の前に `std::isfinite` と範囲を検査し、`clamp` は lo と hi を入れ替えるか失敗にする。

### [Low] プレビューの「書き出しキャンバス枠」とスナップが常に 16:9 で、縦長や正方形のプロジェクトでは書き出し範囲と合わない
- 区画: part04
- 場所: src/GLPreview.cpp:2972-2984(`canvasFrameRect` が 16/9 固定)、:2826-2840(枠の描画)、:3466-3489(スナップの基準)
- 問題: 枠は「書き出される範囲」を示すためのもの(US-T36)ですが、プロジェクトの縦横比(m_displayAspectRatio やプロジェクトの出力サイズ)を見ていません。
- 発生条件: 1080x1920 などの 16:9 以外のプロジェクトで、プレビュー上の映像をドラッグする。
- 影響: 黄色い枠が実際の書き出し範囲と違う位置に出ます。映像は 16:9 の枠の縁にスナップするため、書き出すと意図しない位置になります。
- 確度: 確認済み
- 修正方針: 枠は `letterboxRect()`(プロジェクトの縦横比)と同じ式で作る。

### [Low] Frame.io のコメント取り込みが、タイムスタンプ(フレーム番号)を常に 30fps として秒に直す
- 区画: part04
- 場所: src/FrameIoImporter.cpp:306(`parseFrameIoJson(arr)` を既定の fps=30 で呼ぶ)、:338-342、呼び出し元 src/MainWindow.cpp:18276-18283(fps を渡す手段がない)
- 問題: Frame.io の `timestamp` はフレーム番号として扱われていますが、素材の fps もプロジェクトの fps も使っていません。
- 発生条件: 24 / 25 / 60fps などの素材のコメントを取り込む。
- 影響: コメントの位置がずれます(24fps では 0.8 倍、60fps では 2 倍の時刻になります)。
- 確度: 確認済み
- 修正方針: `fetchComments` に fps を渡し(プロジェクトの fps、可能なら asset の fps)、`parseFrameIoJson(arr, fps)` で使う。

### [Low] Instagram 投稿: 状態確認が 20 秒で打ち切られ、失敗時のメッセージにアクセストークンが出る可能性
- 区画: part05
- 場所: src/InstagramPublish.cpp:117-183, 134-136, 148-151
- 問題: (1) 状態確認は 2 秒 × 10 回 (kMaxPolls) の約 20 秒で "Timed out" にする。Reels の処理は動画の長さによって数十秒〜数分かかることがあり、その間 IN_PROGRESS が続くと失敗扱いになる (作ったコンテナは公開されずに残る)。EXPIRED も ERROR として扱われない。(2) 状態確認の GET は `?fields=status_code&access_token=<token>` を URL に入れている。HTTP エラーのとき QNetworkReply::errorString() は "Error transferring <URL> - server replied: ..." の形でリクエスト URL を含むので、その文字列が publishFailed を経てダイアログのラベル (InstagramPublishDialog.cpp:94-96) に表示される。
- 発生条件: (1) 長めの動画を投稿する。(2) 状態確認で 400 / 401 などの HTTP エラーが返る。
- 影響: (1) 正常に処理中の投稿が失敗扱いになる。(2) 画面共有や録画、スクリーンショットで長期トークンが漏れる。
- 確度: 可能性あり ((1) は Meta 側の処理時間による。(2) は Qt の errorString の書式 (QNetworkReplyHttpImpl が URL を含める) を前提にしている)
- 修正方針: 状態確認は指数バックオフで数分まで続け、EXPIRED も扱う。トークンは Authorization ヘッダで送り、エラー文には URL を含めない (HTTP ステータスとレスポンス本文の error.message だけにする)。

### [Low] LUT の強度スライダーを動かすたびに生成 LUT を上書きし、書き出しがフレームごとに読み直すため、書き出し中に壊れた (途中までの) LUT を読みうる
- 区画: part05
- 場所: src/MainWindow.cpp:467-504 (lutPathForClipExport), 6967-6983 (lutIntensityChanged), TimelineFrameRenderer.cpp:736 (フレームごとの loadCubeFile)
- 問題: 内蔵 LUT は強度を変えるたびに `generated-luts/<name>.cube` を WriteOnly (切り詰め) で書き直す。アトミックな書き込みではない。書き出し側はフレームごとにこのファイルを開いて解析し、行数が合わなければ LUT なしとして扱う。
- 発生条件: 内蔵 LUT を使うクリップを書き出している最中に、カラーグレーディングパネルの LUT 強度を動かす。
- 影響: 一部のフレームだけ LUT が掛からずに書き出される。
- 確度: 可能性あり (書き出しが同じプロセスの別スレッドで同時に動いている場合に限る)
- 修正方針: 内容が同じなら書き直さない。QSaveFile でアトミックに書く。書き出し開始時に LUT を 1 回だけ読み込んでキャッシュする。

### [Low] Lumetri スコープの間引き (100ms) で最後のフレームを捨てるため、一時停止 / シーク後のスコープが表示中のフレームと違う
- 区画: part05
- 場所: src/LumetriScopes.cpp:506-517
- 問題: setFrame は前回の更新から 100ms 未満なら何もせずに return する。あとで反映する仕組み (保留フレームとタイマー) がないため、短い間隔で 2 回シークすると 2 回目のフレームはスコープに反映されず、次のフレームが来るまで (一時停止中はずっと) 前のフレームの波形 / ベクトルスコープが表示される。
- 発生条件: 一時停止中に矢印キーでコマ送りを素早く 2 回行う、または再生を止めた直後。
- 影響: カラリストが見ている波形が画面のフレームと一致しない。
- 確度: 確認済み
- 修正方針: 間引いたフレームを保持し、QTimer::singleShot(残り時間) で最後のフレームを必ず反映する。

### [Low] パーティクル / VFX ジェネレータの生成物が一時フォルダに残り続け、プロジェクトはそのパスを参照する
- 区画: part06
- 場所: src/MainWindow.cpp:14728-14773、:14819-14874 (`QTemporaryDir::setAutoRemove(false)`)
- 問題: 連番 PNG (数百〜数千枚) と出力動画を QTemporaryDir に作って自動削除を切り、PNG を消さない。出力動画はそのままクリップのパスになるが、保存時の集約 (saveProjectToPath の ImportHub 処理) は `v-simple-editor-importhub` 配下しか対象にしないため、プロジェクトの隣にコピーされない。
- 発生条件: パーティクル / VFX ジェネレータを追加して保存し、OS の一時フォルダ掃除や再起動のあとで開く。
- 影響: 一時フォルダに PNG が溜まり続ける。再起動後はクリップがオフラインになる。
- 確度: 確認済み (QTemporaryDir の既定テンプレートと importHubTempDirPath の比較、:869-872)
- 修正方針: PNG はエンコード後に削除し、出力動画は importHubTempDirPath() 配下 (またはプロジェクトの隣) に置いて保存時の集約に乗せる。

### [Low] Mograph テンプレートの追加が引数の並びを取り違え、しかも何も描画しない
- 区画: part07
- 場所: src/MainWindow.cpp:21874 (`tmplate == "lower_third"`。templateNames は "Lower Third")、src/MographText.cpp:130-132 / 256-257 (Title Card は args[0] が色、args[1] が文字)、src/MographText.cpp:400 (renderTemplate の呼び出し元が無い)
- 問題: Lower Third でも引数を 1 個しか集めない。Title Card では入力文字が色 (QColor("Mograph Text") は無効) として扱われ、文字は既定の "Title" になる。m_mographTexts に追加するだけで、プレビューにも書き出しにも描かれない。
- 発生条件: Mograph テンプレートを追加する。
- 影響: ステータスバーに「適用」と出るが何も表示されない。保存されたデータも引数がずれている。
- 確度: 確認済み
- 修正方針: テンプレート名の比較を templateNames と揃え、テンプレートごとに引数の並びを定義して収集する。描画経路 (オーバーレイ化) を実装するか、メニューを無効にする。

### [Low] マスクの feather / expansion に上限が無く、手編集のプロジェクトで描画がほぼ停止する (先頭マスクの合成モードも無視される)
- 区画: part07
- 場所: src/MaskSystem.cpp:213-214 / 323-338 / 412-416 (radius = ceil(amount) を 3 回のボックスブラーに使う)、:458-461。読込は src/mask/ClipMask.cpp:404-413 (clamp なし)
- 問題: feather.amount や expansion が巨大 (例: 1e9) だと boxBlur の初期ウィンドウのループが radius 回まわり、1 行ごとに約 1e9 回 × 高さ × 3 パスになる。inf だと int 変換が未定義動作になる。また generateMaskImage は先頭マスクをモードに関係なく「置き換え」にするため、先頭が Subtract のマスクは形の内側が見え、除外にならない。
- 発生条件: 手編集や外部ツールで作ったプロジェクトの clip mask に大きな feather / expansion、または先頭が subtract のマスクがある。
- 影響: プレビューと書き出しが固まる。マスクの効果が逆になる。
- 確度: 可能性あり (UI から作る値は 0〜100 程度で、到達経路は手編集の JSON のみ)
- 修正方針: 読込時と描画時に feather/expansion を [0, 1000] 程度へ clamp し、非有限値を 0 にする。先頭マスクは白キャンバスに対して combineMasks で合成する。

### [Low] NoiseReduction のフィルタ引数が UI の範囲で FFmpeg の許容範囲を外れ、処理が失敗するか、黙ってパススルーになる
- 区画: part07
- 場所: src/NoiseReduction.cpp:151-157 (afftdn nr = amount*97、nf = noiseFloor)、:168-172 (nlmeans s)、:308-313 / 356-361 (失敗するとそのストリームはフィルタ無しでコピー)。UI は src/MainWindow.cpp:13130-13135 / 13180-13186
- 問題: afftdn の nf の許容範囲は -80〜-20 dB、nr は 0.01〜97 だが、UI は noiseFloor -80〜0、reduction 0.0 を許す。nlmeans の s は 1〜30 だが spatial 0 を許す。グラフを作れないとそのストリームを「フィルタ無し」としてパススルーに回す実装なので、両方を処理する denoiseAll では失敗した側だけ未処理のまま成功と報告される。
- 発生条件: noise floor を -20 より大きく、reduction を 0、または NLMeans の spatial を 1 未満にする。
- 影響: 「Failed to build filter graphs」で失敗する。denoiseAll では一部が未処理のまま成功と出る。
- 確度: 確認済み (各フィルタの範囲は FFmpeg のオプション定義による)
- 修正方針: UI と buildXxxFilterDesc で FFmpeg の範囲へ clamp する。グラフの構築に失敗したらエラーにし、黙ってパススルーにしない。

### [Low] 再リンクダイアログのフォルダ検索が、別 OS で保存されたパス (Windows の `\`) からファイル名を取り出せない
- 区画: part07
- 場所: src/MediaRelinkDialog.cpp:117-119, 131, 158 (QFileInfo(original).fileName())
- 問題: macOS/Linux の QFileInfo は `\` を区切りとして扱わないので、`C:\Users\a\b.mp4` の fileName が文字列全体になり、同名候補が見つからない。
- 発生条件: Windows で保存したプロジェクトを macOS/Linux で開き、「フォルダを指定して検索」を使う。
- 影響: 自動の再リンクができず、1 本ずつ選び直す必要がある。
- 確度: 可能性あり (クロス OS の運用が前提)
- 修正方針: 元パスを QDir::fromNativeSeparators し、さらに `\` を `/` に置き換えてからファイル名を取り出す。

### [Low] パーティクルダイアログのプレビューと実際に生成されるクリップで、粒の大きさと速度の比率が違う
- 区画: part08
- 場所: src/ParticleSystem.cpp:191-201 (`update` は QSize(1,1) → 1920x1080 のフォールバックでシミュレーション)、:348-360 (`renderFrame` は位置だけ縮小し、size は縮小しない)、src/ParticleEffectDialog.cpp:556-570 (320x180 でプレビュー)。生成は src/MainWindow.cpp:14714 (プロジェクト解像度で `renderParticleSequence`)
- 問題: プレビューは 1920 幅の空間で計算し、位置を 1/6 に縮めて描きます。しかし粒の半径はピクセル値のままなので、粒が画面に対して 6 倍大きく見えます。生成するクリップは、プロジェクト解像度の空間で直接計算します (速度 px/s と粒の大きさがプロジェクト画素の単位)。そのため、4K のプロジェクトでは画面に対する速度が半分になり、プレビューと一致しません。
- 発生条件: パーティクル効果のダイアログでプレビューを見てから適用する (特にプロジェクトが 1080p でない場合)。
- 影響: プレビューで調整した見た目と、生成されたクリップの見た目が違います。
- 確度: 確認済み (3 経路のキャンバスの扱いを読み比べました)
- 修正方針: プレビューも `renderParticleSequence` と同じ方式で、プロジェクトのアスペクトのキャンバス上でシミュレーションして縮小表示する。または renderFrame で size にも sx/sy を掛ける。

### [Low] パス上のテキストが文字を QChar 単位で並べるため、サロゲートペアや結合文字が崩れる
- 区画: part08
- 場所: src/PathText.cpp:325-336 (`for (const QChar ch : m_text)` で 1 単位ずつ `addText` と `horizontalAdvance`)
- 問題: BMP 外の文字 (例: 「𠮷」U+20BB7、絵文字) は、上位と下位のサロゲートに分かれて別々のグリフとして描かれ、豆腐 (□) が 2 つになります。結合文字 (例: 「か」+ U+3099 の濁点) も、独立した 1 文字として次の位置に置かれます。
- 発生条件: 「パス上のテキスト」に、人名の異体字や絵文字、NFD の日本語 (macOS のファイル名をコピーした文字列など) を入れる。
- 影響: 文字化けしたテキストがプレビューと書き出しの両方に出ます。
- 確度: 確認済み (ループの単位を読みました)
- 修正方針: `QTextBoundaryFinder` (Grapheme) で書記素クラスタごとに分けて、各クラスタの advance と path を作る。

### [Low] 「保存版と比較」が、カラーのオフセット (offsetR/G/B) とエフェクトのマスクの変更を検出しない
- 区画: part08
- 場所: src/ProjectDiff.cpp:141-170 (`grade` オブジェクトで colorCorrection を置き換えるが、offsetR/G/B を含めていない)、:222-227 (effects を置き換えるが、`mask` を含めていない)
- 問題: 永続化の JSON にある colorCorrection.offset* と effects[].mask を、比較用の射影で捨てています。
- 発生条件: カラーホイールのオフセットだけ、またはエフェクトマスクだけを変えてから、保存版と比較する。
- 影響: 「変更はありません」と表示され、差分を見落とします。
- 確度: 確認済み (射影のキーの一覧と ProjectFile::colorCorrectionToJson / effectToJson を突き合わせました)
- 修正方針: grade に offsetR/G/B を加え、effects の射影に `mask` (`e.mask.toJson()`) を加える。

### [Low] オブジェクト除去の適用を中止・失敗したとき、途中まで書いた PNG 連番のフォルダが素材フォルダに残る。処理中に Esc / × で閉じても処理が止まらない
- 区画: part08
- 場所: src/ObjectRemovalDialog.cpp:370-433 (中止や失敗で return するとき、`folder` を消さない)、:324-347 (出力先は素材と同じフォルダの `<name>_object_removal[_N]`)、:448-456 (中止は「閉じる/中止」ボタンだけ。QDialog 標準の Esc / closeEvent は reject になり、`m_cancelRequested` を立てない)、:282-288 (`setBusy(false)` が、setContext で無効にした「適用」を無条件に有効化する)
- 問題: 試行するたびに `_object_removal_2`、`_3`… という不要なフォルダが増えます。処理中に Esc を押すとダイアログは隠れますが、ループは続き、最後に取り込みと `accept()` まで実行されます。
- 発生条件: 長い範囲で「適用」を押してから中止する、途中でマスクの取得に失敗する、または処理中に Esc を押す。
- 影響: 素材フォルダに大量のゴミ PNG が残ります。閉じたつもりのダイアログの処理が裏で続き、新しいシーケンスが追加されます。
- 確度: 確認済み (onApplyClicked / onCancelClicked / setBusy を読みました。Esc の扱いは QDialog の標準動作です)
- 修正方針: 中止や失敗のときは出力フォルダを `removeRecursively` する。`reject()` / `closeEvent` をオーバーライドし、処理中は中止要求に変える。setBusy(false) では setContext と同じ条件で「適用」を有効にする。

### [Low] PPTX の書き出しとプロジェクトの収集が、途中で失敗すると壊れたファイルを残す
- 区画: part08
- 場所: src/PptxExportDialog.cpp:204-221 (既存の .pptx を `Truncate` で開いて直接書く)、src/ProjectCollector.cpp:222-227 (書き込みが short write のとき、途中まで書いたコピー先を消さずに return する)
- 問題: どちらも書き込みがアトミックではありません。PPTX は、既存のファイルを上書きしている途中で失敗すると、元のファイルが失われます。収集は、途中まで書いたメディアが `media/` に残ります。次回の収集では名前が衝突したと判定されて `_2` が付くため、中身が欠けたファイルが残り続けます。
- 発生条件: 書き出し先や収集先のディスクの空き容量不足、ネットワークドライブの切断。
- 影響: 既存の資料の消失や、壊れたメディアファイルが残ります。
- 確度: 確認済み (コードを読みました。ProjectFile::save は QSaveFile を使っています)
- 修正方針: PPTX は `QSaveFile` で書いて commit する。収集の short write の分岐でも `dstFile.close(); QFile::remove(destPath);` を行う。

### [Low] プロキシ完了時のフィンガープリントと mtime が、ジョブ開始時ではなく完了時点の設定・時刻で記録される
- 区画: part09
- 場所: src/ProxyManager.cpp:998-1002
- 問題: 完了 lambda の `computeConfigFingerprint(m_config, activeEncoder, m_qualityPreset)` は、エンコード中に変更された現在の設定を使います。実際のエンコードには `job.config` / `job.preset` が使われています。`sourceMtimeMs` も完了時点の mtime を読みます。
- 発生条件: プロキシ生成中に品質プリセットを変える、または生成中にソースファイルが更新される。
- 影響: 古いプリセットで作ったプロキシが、新しいプリセットのものとして「最新」扱いになります。生成中に更新されたソースも stale と判定されません。
- 確度: 確認済み
- 修正方針: フィンガープリントは job.config / job.preset から作る。mtime は processNextInQueue の時点で取って job に持たせる。

### [Low] Python コンソールで、出力が 3 回、エラーが 2 回表示される
- 区画: part09
- 場所: src/PythonScript.cpp:270-282, 447-450, 473-477, 504-505 / 872-878
- 問題: print の出力は、Tee の "output" メッセージの段階で `outputReady` として表示されます。さらに `allOutput` にも足されます。result メッセージの `output`(同じ内容を `_captured_out` にためたもの)も `allOutput` に足されます。onRun は最後に `result.output` をもう一度表示します。stderr は "output" として出たうえに、`errorOccurred` と onRun の `appendOutput(result.error)` でも表示されます。
- 発生条件: print を含むスクリプトを実行する。
- 影響: コンソールに同じ出力が繰り返し並び、ログを読むのが困難になります。
- 確度: 確認済み
- 修正方針: 逐次表示に一本化し、result の output は足さない。onRun では output を再表示しない。

### [Low] Python ヘルパスクリプトを、共有の一時フォルダの固定名で作り、そのまま実行する
- 区画: part09
- 場所: src/PythonScript.cpp:308-316, 386, 328-329
- 問題: `TempLocation + "/veditor_python_helper.py"` に書き込み、実行のたびに python がこのパスを読み込みます。Linux の `/tmp` はユーザー間で共有されます。そこに他ユーザーが同じ名前のファイルを先に置いておく(書き込み権限つき)か、書き込みの後に中身を差し替えると、そのコードが編集ソフトのユーザー権限で動きます。また、2 つ目のインスタンスが終了時に `shutdown()` でこのファイルを消すと、1 つ目のインスタンスのスクリプト実行がすべて失敗します。
- 発生条件: マルチユーザーの Linux。またはアプリを 2 つ同時に起動する。
- 影響: ローカルの他ユーザーによるコード実行。多重起動時のスクリプト機能の停止。
- 確度: 可能性あり(悪用には共有 /tmp と他ユーザーの存在が要る。Windows / macOS の TEMP はユーザーごと)
- 修正方針: QTemporaryFile(一意名、0600)で作るか、AppDataLocation 配下に置く。

### [Low] Remotion 出力で文字列をエスケープしておらず、Windows では npm / npx / code を起動できない
- 区画: part09
- 場所: src/RemotionExport.cpp:434(`name="` << fi.baseName())、437, 535, 761, 868(`src` が未エスケープ)、271(package.json の name)/ 1680-1686, 1656, 1776(QProcess "npm" / "npx" / "code")
- 問題: テキストには `tsStringLiteralContent` を通していますが、src パス、JSX の name 属性、package.json の name には通していません。ファイル名やプロジェクト名に `"` や `\` があると、生成される TSX / JSON の構文が壊れます。また Windows の npm / npx / code の実体は `.cmd` なので、QProcess の start / startDetached ではそのまま起動できません。
- 発生条件: ファイル名やプロジェクト名にダブルクォートがある。または includeAssets を外していて Windows パスに `\u…` が含まれる。Windows で「npm install」や「VS Code で開く」を選ぶ。
- 影響: 生成したプロジェクトがコンパイルできません。npm install が常に「failed」になり、60 秒間 UI が止まります。
- 確度: 確認済み(.cmd の点は Qt の CreateProcess の仕様による)
- 修正方針: 埋め込む文字列はすべて tsStringLiteralContent / JSON エスケープを通す。Windows では `cmd /c` 経由で起動する。

### [Low] Track Shape の後にパスを編集すると、OK 時にその編集が捨てられ、既存のキーフレームも全部置き換えられる
- 区画: part09
- 場所: src/RotoToolsDialog.cpp:659-678(`m_trackedKeyframes` が後の編集で消えない)、382-385, 552-563, 652-656 / src/MainWindow.cpp:15034-15036
- 問題: Track Shape の結果は、そのあと Auto-Trace、点のドラッグ、スナップでパスを変えても `m_trackedKeyframes` に残ります。MainWindow は `trackedKeyframes()` が空でなければ `entry.keyframes = keyframes` とします。キーフレームは単体のパスより優先されるので(RotoApply.cpp:46)、後からの編集は反映されません。この代入で、追跡範囲(最大 8 フレーム)の外にあった既存のキーフレームもすべて消えます。
- 発生条件: Track Shape → パスを修正 → OK。または、キーフレームのあるクリップで Track Shape を実行する。
- 影響: ロトの編集とキーフレームが黙って失われます。
- 確度: 確認済み
- 修正方針: パスを変更したら m_trackedKeyframes を消す(または追跡し直しを促す)。保存側は、追跡したフレーム範囲だけを置き換えてマージする。

### [Low] プロジェクトファイルのロト feather に上限がなく、巨大な値で描画が止まる
- 区画: part09
- 場所: src/Rotoscope.cpp:73, 351-357, 297-301, 317-319
- 問題: `RotoPath::fromJson` は `feather` をそのまま読みます。`estimateFeather` はこれを `int radius = ceil(feather)` にして boxBlur を 3 回かけます。boxBlur は行と列ごとに初期化ループを `radius` 回まわすので、計算量が O(radius × (W+H))/フレーム になります。INT_MAX を超える値では、int への変換が未定義動作になります。
- 発生条件: `"feather": 1e9` などを含む .veditor を開き、そのクリップをプレビューまたは書き出しする。
- 影響: UI スレッド(プレビュー)や書き出しのスレッドが事実上止まります。
- 確度: 確認済み
- 修正方針: 読み込み時と描画時に、feather を有限かつ [0, max(W,H)] の範囲に clamp する。初期化ループは `min(radius, w-1)` で打ち切る。

### [Low] Whisper の種類判定がパスの部分一致で、"main" を含むパスの Python 版 whisper を whisper.cpp と取り違える
- 区画: part10
- 場所: src/SubtitleGenerator.cpp:308-310, 431-433
- 問題: `whisperBin.contains("main")` をフルパスに対して調べている。/home/maintainer/.local/bin/whisper や /Users/romain/... のようにパスのどこかに "main" があると、openai-whisper に whisper.cpp 用の引数 (-f, --output-json, -of) を渡してしまう。
- 発生条件: ユーザー名やディレクトリ名に main を含む環境で、Python 版 whisper を使う。
- 影響: 字幕生成が毎回 "Whisper failed" になる。
- 確度: 確認済み
- 修正方針: QFileInfo(bin).fileName() で判定するか、findWhisperBinary から種類も一緒に返す。

### [Low] 速度ランプ曲線の逆変換 sourceToTimelineUs が、最初のキーが 0 より後にあると区間の開始時刻を足し忘れる
- 区画: part10
- 場所: src/SpeedRampData.cpp:136-175
- 問題: 部分区間の戻り値が `timelineAccum + x` になっている。timelineAccum は区間を丸ごと通過したときにだけ segEnd に更新されるので、最初の区間 (segStart > 0) では 0 のままになり、segStart を足していない。順変換 timelineToSourceUs は最初のキーより前ではソースを進めないので、往復が一致しない。
- 発生条件: speedRamp の先頭キーが 0 より後にあるプロジェクト (手で編集した JSON、外部ツールで作ったもの。fromJson はソートするだけで 0 のキーを補わない) で、対象のソース時刻が最初の区間に入る。
- 影響: ClipInfo::localSecondAtSourceTime (Timeline.cpp:150-155) と VideoPlayer::fileLocalToTimelineUs が segStart 分だけ早い時刻を返し、再生位置やマーカーの対応がずれる。
- 確度: 確認済み (手計算: キーが (1s,1x),(2s,1x) のとき timelineToSource(1.5s)=0.5s、sourceToTimeline(0.5s)=0.5s で、正しくは 1.5s)
- 修正方針: 部分区間では `segStart + x` を返す。fromJson で先頭キーが 0 より後なら 0 秒のキーを補う。

### [Low] スマートレンダーの丸ごとコピー判定に、カーブ・HSL・wiggle・式・ブレンド・調整レイヤー・パペット・メッシュ・クリップ EQ・チャンネルモードの除外がない
- 区画: part10
- 場所: src/SmartRender.cpp:419-455 (hasAudioProcessing / specialOverlayReasonForClip)、461-552 (canStreamCopy)
- 問題: 画が変わる ClipInfo の項目のうち、colorCurves、hslSecondary、wiggle、expressions、blendMode、isAdjustment、isVfxFootage、puppetPins、meshWarp、shapes を見ていない。音が変わる項目のうち、clipEq と audioChannelMode を見ていない。AUDIT-REPORT が挙げる逆再生とタイムリマップとは別の漏れ。
- 発生条件: VEDITOR_SMART_RENDER=1 で、トラック 0 にクリップ 1 本だけのタイムラインを、上の項目のどれかだけを設定して書き出す。
- 影響: グレード、揺れ、EQ などが黙って落ちた原本コピーが出力される。
- 確度: 確認済み (オプトインの機能)
- 修正方針: 「既定値かどうか」の判定を ClipInfo 側に一本化して (isRenderIdentity() など)、そこから判定する。

### [Low] ShortcutCustomizeDialog で読めない文字列を入力すると、警告なしにショートカットが消える (macOS では表示文字列そのものが読めない)
- 区画: part10
- 場所: src/ShortcutCustomizeDialog.cpp:118, 202-217
- 問題: 表と入力欄の初期値は NativeText (macOS では ⌘⇧O のような記号) なのに、入力は `QKeySequence(str)` (PortableText) で読む。読めない文字列 ("Ctrl+Shft+O" や ⌘ 記号) は空のシーケンスになるが、空かどうかは元の文字列でしか確認していないので、setBinding が空を設定してしまう。
- 発生条件: macOS で既存の値を編集せずに OK を押す。または打ち間違える。
- 影響: ショートカットが黙って消える。
- 確度: 確認済み
- 修正方針: 入力には QKeySequenceEdit を使う。少なくとも解析後の isEmpty() と、NativeText での再解析を確認する。

### [Low] SRT/VTT の書き出しが Windows で改行を "\r\r\n" にする
- 区画: part10
- 場所: src/SubtitleIO.cpp:269, 277-281, 296, 302-308
- 問題: QIODevice::Text で開いたまま、明示的に "\r\n" を書いている。Windows では Text モードが '\n' を "\r\n" に変換するので、"\r\r\n" になる。
- 発生条件: Windows で字幕を書き出す (CaptionEditorDialog の書き出し、RenderQueue の mov_text 用の一時 SRT)。
- 影響: 他のツールで空行やゴミ文字が出ることがある。FFmpeg の SRT 読み込みへの影響は可能性あり。
- 確度: 確認済み (書き込まれるバイト列)
- 修正方針: Text フラグを外して "\r\n" を明示するか、Text のまま "\n" だけを書く。

### [Low] 3D 押し出しテキストの太字・斜体などのフォントスタイルが保存されず、ダイアログのプレビューとタイムラインで見た目が違う
- 区画: part10
- 場所: src/Text3DLayer.cpp:648-651, 681-691 / src/Text3DExtrusionDialog.cpp:396, 485-492 / src/MainWindow.cpp:15494-15501
- 問題: Text3DLayer::toJson はフォントの family と pointSize しか書かない。QFontDialog で太字を選ぶと、ダイアログのプレビュー (m_currentFont をそのまま使う) は太字になるが、commitText3DConfig に渡る JSON は太さと斜体を失う。描画 (applyClipText3D の fromJson) は標準の太さになる。
- 発生条件: 3D テキストで太字、斜体、ウェイト違いのフォントを選ぶ。
- 影響: プレビューと書き出しの両方で、選んだスタイルと違う見た目になる。
- 確度: 確認済み
- 修正方針: QFont::toString() (または weight / italic / styleName) を JSON に保存して復元する。

### [Low] 3D テキストのベベル分割数をプロジェクトの値のまま使うので、大きな値でメモリを使い果たす
- 区画: part10
- 場所: src/Text3DLayer.cpp:715 / src/ExtrudedMesh.cpp:196-221
- 問題: fromJson は extrudeBevelSegments を範囲検査せずに読む (ダイアログの範囲は 0〜8)。buildExtrudedMesh は 2*(segs+1) 段のプロファイルにリングの頂点数を掛けた頂点を作る。
- 発生条件: text3DConfig の extrudeBevelSegments が大きい値 (例えば 100000000) のプロジェクトを開いて再生または書き出す。
- 影響: 確保に失敗してクラッシュするか、長時間固まる (細工したプロジェクトファイルで起こせる DoS)。
- 確度: 確認済み
- 修正方針: fromJson で [0, 8] にクランプする。depth と bevel も有限で正の範囲にクランプする。

### [Low] Smart Edit Assistant の「タイムラインに適用」が何もせず、エラーもログに出るだけ
- 区画: part10
- 場所: src/SmartEditDialog.cpp:154-165 / src/SmartEditAssistant.cpp:195-239
- 問題: onApplyToTimelineClicked は qInfo を出すだけ。解析の失敗は qWarning だけで、ユーザーには何も表示されない。analyze() は GUI スレッドで同期的に音声と映像を全部解析するので、その間 UI が止まる。dropShortClips は 1 秒未満の発話をはさんだ無音やシーンチェンジの点を 1 つの「カット範囲」に統合するので、適用を実装した場合は発話を削ってしまう。
- 発生条件: ダイアログで解析して「Apply to Timeline」を押す。
- 影響: ボタンが反応せず、結果も出ない。
- 確度: 確認済み
- 修正方針: 未実装のあいだはボタンを無効にする。失敗はダイアログに表示する。解析はワーカースレッドで行う。

### [Low] SNS 書き出しダイアログのリフレーミング設定・fps・ビットレートが反映されない
- 区画: part10
- 場所: src/SocialExportDialog.cpp:183-193, 252-256 / src/MainWindow.cpp:17171-17176
- 問題: 受け側は `Q_UNUSED(reframeParams)` で、解像度だけを ProjectConfig に入れる。モード、中心、ズームのスライダーも、プリセットの targetFps、videoBitrateBps、maxDurationSec も使わない。m_sample は一度も設定されないので、reframeParams().sourceSize も常に空になる。
- 発生条件: SNS プリセットを適用する。
- 影響: UI で選んだクロップ位置やズームが効かない。
- 確度: 確認済み
- 修正方針: 使わない UI は外すか、reframe::ReframeParams を実際の配置 (videoDx / videoScale など) に変換して適用する。

### [Low] シェイプのグラデーション塗りで不透明度が無視される
- 区画: part10
- 場所: src/ShapeLayer.cpp:520-535
- 問題: 単色の塗りは `color.alphaF() * fill.opacity` を使うが、グラデーションの経路 (createGradientBrush) は fill.opacity を掛けない。
- 発生条件: グラデーション塗りのシェイプで opacity を 1 未満にする。
- 影響: 不透明度を下げても変わらない。
- 確度: 確認済み
- 修正方針: グラデーションの 2 色の alpha にも fill.opacity を掛ける。

### [Low] テキストアニメーションのプリセットが、前に適用したプリセットの設定を引き継ぐ
- 区画: part10
- 場所: src/TextAnimPresets.cpp:81-253
- 問題: animator の現在の cfg、range、wiggly を土台に、プリセットごとに一部の項目だけを上書きする。例えば "Spin In Each Word" (basedOn=Words) の後に "Typewriter Sound" を適用すると Words 単位のまま。"Decode" (wiggly 有効) の後に "Stomp" を適用すると wiggly が残る。customColor、reverse、amplitude も同じ。
- 発生条件: プリセットを続けて適用する。
- 影響: 同じプリセットでも、適用する順番によって結果が変わる。
- 確度: 確認済み
- 修正方針: 既定値から組み立ててからプリセットの値を設定する。

### [Low] スチル一覧 (index.json) が壊れると、新しいスチルも保存できなくなる
- 区画: part10
- 場所: src/StillStore.cpp:124-129, 227-233
- 問題: list() は JSON の解析に失敗するとエラーを返し、save() はそのエラーで中断する (保存した PNG も消す)。修復や退避の経路がない。
- 発生条件: 書き込み中の電源断などで index.json が壊れる (QSaveFile なので起きにくいが、手で編集したときなど)。
- 影響: ユーザーが index.json を手で消すまで、スチルを保存できない。
- 確度: 確認済み
- 修正方針: 解析に失敗したら index.json を .bak へ退避し、ディレクトリの still-*.png から一覧を作り直す。

### [Low] setup.bat の FFmpeg に x264 / x265 が入らず、build.bat と書き出しの能力が違う
- 区画: part11
- 場所: setup.bat:223 (`ffmpeg[core,avformat,avcodec,swscale,swresample,avfilter]`) と build.bat:104-107 (x264,x265,opus,mp3lame 付き)
- 問題: ワンクリックセットアップで作ったビルドには libx264 / libx265 / libopus / libmp3lame が無い。書き出しやプロキシの既定は libx264(BatchExportQueue.cpp:43-53、MainWindow.cpp:13893 の「CPU (libx264)」)。
- 発生条件: setup.bat で作ったビルドで H.264 の CPU エンコード、HEVC、Opus、MP3 を書き出す。
- 影響: 書き出しでエンコーダが見つからず失敗するか、h264_mf などの別エンコーダに落ちて画質やビットレートの挙動が変わる。
- 確度: 可能性あり(h264_mf へのフォールバックは ProxyManager.cpp:81 にはあるが、書き出し経路の全部は追っていない)
- 修正方針: setup.bat の FFmpeg features を build.bat と揃える(vcpkg.json に寄せる)。

### [Low] setup.bat が配布・ショートカットの対象に、今ビルドした Release ではなく古い Debug などの exe を選ぶ
- 区画: part11
- 場所: setup.bat:295-300
- 問題: `for %%d in (Release RelWithDebInfo Debug MinSizeRel)` で、見つかるたびに EXE_PATH / EXE_DIR を上書きする。見つかった中で最後のもの(Debug や MinSizeRel)が残る。
- 発生条件: build\ に以前の Debug / MinSizeRel / RelWithDebInfo の exe が残っている状態で setup.bat を実行する(Step 5 で作るのは Release だけ)。
- 影響: windeployqt --release と FFmpeg DLL のコピーが古い Debug ディレクトリに行き、デスクトップのショートカットと「今すぐ起動」も古い exe を指す。今ビルドした Release には Qt DLL が配置されない。
- 確度: 確認済み
- 修正方針: Release を見つけたらループを抜ける(goto で抜ける)か、Release を固定で使う。

### [Low] テキスト overlay の焼き込みで、行間 0 のときは改行入りテキストが 1 行に潰れる(行間が 0 以外なら改行が効く)
- 区画: part11
- 場所: src/TextOverlayBake.cpp:309-312 / 388-393
- 問題: lineSpacing == 0 の経路では、枠の大きさを `fm.boundingRect(ov.text)`(改行を処理しない単一行版)で測り、`QPainterPath::addText(baselineX, baselineY, font, ov.text)` で描く。addText は QTextLayout の 1 行だけを作って全アイテムを同じベースラインに並べるので、'\n' は捨てられて次の行が同じ行に続く。lineSpacing != 0 の経路は layoutBakeTextLines で行に分けるので、行間を 0 にするか 0.1 にするかで改行が効くかどうかが変わる。wordWrap=true も、自動幅のときは効かない。
- 発生条件: MCP の text overlay ツールなどで "1行目\n2行目" を渡し、行間を既定の 0 のままにする。
- 影響: プレビューと書き出しの両方(共通の bakeOverlays)で、2 行の字幕・タイトルが 1 本の長い行として描かれ、画面からはみ出す。
- 確度: 可能性あり(Qt の QPainterPath::addText の実装が単一行であることにもとづく。実行しての確認はしていない)
- 修正方針: lineSpacing == 0 でも layoutBakeTextLines / spacedBakeTextPath(lineSpacing 0)を使い、行分割と配置を 1 つの経路にまとめる。

### [Low] 字幕 (SRT / VTT) の読み込みがクリップに反映されず、パーサーにも末尾の字幕の重複と VTT の cue 設定の誤読がある
- 区画: part11
- 場所: src/MainWindow.cpp:12392-12397 (呼び出し側)、src/TextManager.cpp:255-275 (importSRT)、src/TextManager.cpp:316-332 (importVTT)、src/TextManager.cpp:293-300 (VTT parseTime)
- 問題: (1) 呼び出し側は `auto clips = m_timeline->videoClips();` のコピーに addOverlay するだけで、タイムラインへ書き戻さない(コメントは "// Update via timeline")。それでも「Imported N subtitle(s)」と表示する。(2) パーサーは空行で cue を確定しても textLines をクリアしない。ファイルが通常どおり空行で終わると、ループ後の「Handle last entry」で最後の字幕をもう一度、startTime=0 / endTime=0(0 は「最後まで」)で追加する。(3) VTT の `00:00:02.000 align:start` のような cue 設定付きの行では、`times[1]` を ':' で分けると 4 要素になって 0.0 を返すため、endTime=0(最後まで表示)になる。
- 発生条件: (1) は字幕を読み込むたび。(2)(3) は (1) を直した時点で表に出る。
- 影響: 今は読み込みが黙って何もしない。(1) を直すと、最後の字幕が動画の先頭から最後までずっと表示され、cue 設定付きの VTT はすべての字幕が最後まで残る。
- 確度: 確認済み(コード上で追跡した)
- 修正方針: Timeline 経由(setClipTextOverlays 相当)で書き戻し、undo を積む。空行で確定したら textLines.clear() する。時刻はトークンの先頭の空白までだけを解析する。

### [Low] 「テキストアニメーション追加」「マスクテキスト reveal 追加」「ベンド/インフレート text 追加」が、何も描画も保存もしないのに成功と表示する
- 区画: part11
- 場所: src/MainWindow.cpp:14878-14899 (addTextAnimation)、src/MainWindow.cpp:21770-21776 (TextMaskReveal)、src/MainWindow.cpp:21803-21810 (TextPathWarp)、src/TextPathWarp.cpp:219-224
- 問題: addTextAnimation は TextAnimator をローカル変数で作って捨てる。TextMaskReveal / TextPathWarp は m_textMaskReveals / m_textPathWarps に積むだけで、描画経路(renderFrame の呼び出し)にも ProjectFile の保存(textMaskRevealToJson などはあるが MainWindow から呼ばれない)にもつながらない。TextMaskReveal は親を持たない QObject なので解放もされない。TextPathWarp の inflate はダイアログでは -1〜1 の「量」として入力させるが、warpPoint はその値をそのまま拡大率にする(0 は 1 倍扱い、0.1 は 10% に縮小、負の値は点対称に反転)。
- 発生条件: 該当のメニューを使う。
- 影響: ステータスバーに「追加」と出るのに画面にも書き出しにも何も出ず、保存しても残らない。将来描画につないだ場合、inflate 0.1 で文字が 1/10 に縮む。
- 確度: 確認済み
- 修正方針: 未実装のメニューは隠すか無効にする。つなぐ場合は inflate を `scale = 1 + amount` にする。

### [Low] realfootage_parity.sh の selftest 一覧が古く、登録の無い 7 項目は GUI が起動したまま 120 秒の timeout で FAIL になる
- 区画: part11
- 場所: scripts/realfootage_parity.sh:93-145, 163-167
- 問題: 一覧はコメントで「grep で動的に作る」と書いてあるが、実際は手書きの固定配列。VEDITOR_TRACKMATTE_EXPORT_INTEGRATION_SELFTEST はソースに存在しない。VEDITOR_BRUSH / EFFECT_PARAM_SCHEMA / NODEGRAPH / PLANAR_TRACKER / SNSPACK は CMake のコンパイル時オプション、VEDITOR_TRACKMATTE_SELFTEST は観測用のフラグで、どれも SelftestRegistry にエントリが無い。これらの環境変数を付けて exe を起動しても selftest は走らず、通常の GUI が開く。timeout が終了させるのは WSL 側の cmd.exe だけで、Windows の GUI プロセスは残る可能性がある。
- 発生条件: このハーネスを実行する。
- 影響: 素材が正常でも毎回 FAIL(exit 1)になり、実行するたびにエディタのウィンドウが残る可能性がある。
- 確度: 確認済み(一覧と src/selftests/SelftestRegistry.cpp の env 名を突き合わせた)。プロセスが残る点は可能性あり
- 修正方針: `--selftest=list` の出力から一覧を作る。終了は taskkill /T で子プロセスごと行う。

### [Low] 音量エンベロープの点を右クリックで削除しても undo 点が作られない
- 区画: part12
- 場所: src/Timeline.cpp:3268-3275 (tryHitEnvelopeKeyframe の RightButton)
- 問題: 点を削除したあと `emit modified()` しか出さない。modified は onTrackModified (:13683) に繋がり、undo を保存しない。undo を保存するのは interactionCompleted (:12978-12986) だけである。右ボタンの release はどの drag 状態にも当たらないので、interactionCompleted も出ない。
- 発生条件: エンベロープ編集モードで音声クリップの点を右クリックして消す。
- 影響: Ctrl+Z でこの削除だけを戻せず、1 つ前の操作と一緒に巻き戻る (:12980 のコメントが防ごうとしている症状そのもの)。
- 確度: 確認済み
- 修正方針: 削除の直後に `emit interactionCompleted(QStringLiteral("Delete volume keyframe"))` を出す。

### [Low] 終了時刻なし (endTime=0、クリップ末尾まで) のテキスト overlay の左端をドラッグすると、0〜0.01 秒に潰れる
- 区画: part12
- 場所: src/Timeline.cpp:1244-1245 (TextStripWidget::mouseMoveEvent)、呼び出し先 src/Timeline.cpp:9520-9521 (updateTextOverlayTime)
- 問題: 左端のドラッグは `startTime = qMin(tSec, endTime - 0.1)` で計算する。endTime=0 (Overlay.h:26「0 = until end of clip」、既定値) のとき startTime は -0.1 になる。updateTextOverlayTime はこれを [0, 0.01] に丸めて採用する。以後のドラッグでも endTime=0.01 から抜け出せない。描画 (rectForOverlay:1366) と本体ドラッグ (:1345) は endTime<=0 を「末尾まで」として扱っているのに、左端のドラッグだけがそれを扱っていない。
- 発生条件: endTime を設定していないテキスト overlay の T 行バーの左端をドラッグする。
- 影響: 字幕が 10 ms の表示に変わる (undo では戻せる)。
- 確度: 確認済み
- 修正方針: 左端のドラッグでは、endTime<=0 のとき実効終了 (qMax(start+1, m_clipDuration)) を上限に使い、endTime は 0 のまま渡す。または updateTextOverlayTime 側で endTime<=0 を保持する。

### [Low] トラックマットがあるフレームでは、V1 の調整クリップが書き出しで掛からない
- 区画: part13
- 場所: src/TimelineFrameRenderer.cpp:2537-2541 (V1 調整クリップを trackIndex 0 で activeAdjustments に積む)、3317-3320 (マット経路の適用ループは t = 1 から)
- 問題: マットが無い経路は v1Clip.isAdjustment のとき applyAdjustmentTrack(0) で V1 の調整を掛ける (3087-3088)。マット経路は t = 1 から回すので、trackIndex 0 の調整を一度も適用しない。
- 発生条件: V1 に調整クリップがあり、同じフレームに有効なトラックマットがある。
- 影響: そのフレームだけ V1 の調整レイヤー (エフェクト) が書き出しから消え、マットが始まる・終わる所で絵が飛ぶ。
- 確度: 確認済み
- 修正方針: マット経路のループを t = 0 から回す (またはマットが無い経路と同じく最後に trackIndex 0 を適用する)。

### [Low] 音声クリップのノーマライズが変更前に undo 点を積み、redo で戻せず、次の undo でまとめて消える
- 区画: part13
- 場所: src/Timeline.cpp:12716-12721
- 問題: UndoManager はスタックの先頭を「現在の状態」として扱い、ほかの編集は変更してから saveUndoState する。ノーマライズだけは saveUndoState("Normalize audio clip") を先に呼び、その後で volume を変える。そのため、変更後の状態はどの履歴にも無い。
- 発生条件: 右クリック →「ノーマライズ」の後に redo する、または別の編集をしてから undo する。
- 影響: redo してもノーマライズ後の音量に戻らない。次の操作を undo すると、ノーマライズも一緒に消える。
- 確度: 確認済み (UndoManager::saveState / undo / redo の stack の扱いを確認)
- 修正方針: setClips の後に saveUndoState を呼ぶ (MainWindow.cpp:20544 付近の同名処理も同じ順序か確認する)。

### [Low] ボイスオーバーをクリップの途中に挿入すると、後ろの音声がずれ、右片のエンベロープもずれる
- 区画: part13
- 場所: src/Timeline.cpp:12433-12454 (insertAudioClipAtPlayhead の割り込み分岐)、12386-12414 (ロック確認なし)
- 問題: 再生ヘッドがクリップの内側にあると、左片 + 新規クリップ + 右片 を leadIn 0 で並べる。後ろのクリップはすべて、ボイスオーバーの長さぶん後ろへずれる。右片は ClipInfo をそのままコピーして inPoint だけ変えるので、volumeEnvelope / keyframes / 逆再生の向きは split の規則 (Wave 18: 右片の時刻を切り位置ぶんずらす) を通らない。トラックのロックも確認しない。
- 発生条件: 既存の音声があるトラック番号を指定してボイスオーバーを録音・挿入し、再生ヘッドがクリップの内側 (端から 0.1 秒より内) にある。
- 影響: そのトラックの後続音声がリンク映像とずれる。右片の音量エンベロープが違う位置で鳴る。ロック中のトラックが編集される。
- 確度: 確認済み
- 修正方針: 割り込みは TimelineTrack::splitClipAt を使い、後続は insertClipPreservingDownstream で位置を保つ (または挿入を拒否する)。先頭でロックを確認する。

### [Low] 元動画の字幕をタイムラインに写すとき、逆再生のクリップでは時刻が反転しない
- 区画: part13
- 場所: src/Timeline.cpp:9053-9071 (sourceSecondToClipLocal)、9190-9201 (mapSourceCaptionTrackToTimeline)
- 問題: ソース秒からクリップ内の秒への変換は (source - inPoint) / speed だけで、clip.reversed を見ない (ClipInfo::localSecondAtSourceTime は使っていない)。逆再生クリップは in と out の間で折り返して再生されるので、字幕が鏡写しの位置に置かれる。
- 発生条件: V1 の逆再生クリップの元動画を選び、その字幕をタイムラインへ適用する。
- 影響: 字幕の表示時刻が、実際に話している区間とずれる (区間を左右反転した位置になる)。
- 確度: 確認済み
- 修正方針: clip.localSecondAtSourceTime を使うか、reversed のときは local = effectiveDuration - local に折り返す。単語の順序も逆になるので、区間単位にフォールバックする。

### [Low] 「可変フォント axis アニメ」が何も作らない(開始/終了値を捨て、KeyframeManager も保存も描画もない)
- 区画: part14
- 場所: src/VariableFontAxis.cpp:110-127(fontAt)、呼び出し側 src/MainWindow.cpp:21825-21862
- 問題: `fontAt` は `m_keyframeManager` が null のとき base font をそのまま返す。MainWindow は `setKeyframeManager` を呼ばず、ダイアログで入力した開始値・終了値はステータス表示に使うだけでキーフレームにならない。作った VariableFontAxis は `m_variableFontAxes` に追加されるだけで、どこからも読まれず、保存もされない。
- 発生条件: メニューの「可変フォントaxisアニメ」で値を入れて OK を押す。
- 影響: 「wght 400→700」と表示されるのに、プレビューにも書き出しにも何も反映されない。
- 確度: 確認済み(`m_variableFontAxes` の参照が追加の 1 か所だけであることを grep で確認)
- 修正方針: 開始/終了値でキーフレームを作り、テキストオーバーレイと結びつけて描画と保存に乗せる。それができないなら、メニューを外す。

### [Low] VST/AU スキャナが bundle 形式のプラグインを見つけられない(macOS では 0 件)。uniqueId の衝突やインスタンスのリークもある
- 区画: part14
- 場所: src/VSTHost.cpp:507-513(`QDir::Files` で走査)、465/485(`uniqueId = "vst3:" + baseName`)、576(`m_loadedPlugins << instance`)、957-962
- 問題: VST3 と AU は通常 `Foo.vst3/` や `Foo.component/` というディレクトリ(bundle)だが、走査は `QDir::Files` なので bundle 自体は列挙されない。macOS の bundle 内の実体ファイル(Contents/MacOS/Foo)には拡張子がないため、macOS ではプラグインが 1 件も見つからない。Linux では `.so` を全部プラグインとみなす。uniqueId は baseName から作るので、別フォルダにある同名プラグインは同じ id になり、`onPluginSelected` は常に最初の 1 件を選ぶ。「Load」を押すたびに singleton の `m_loadedPlugins` に追加され、解放されない(dlopen したハンドルが溜まる)。
- 発生条件: macOS で Scan を押す。または、同名プラグインが ~/.vst3 と /usr/lib/vst3 の両方にある。または Load を何度も押す。
- 影響: README が謳う「VST3 / AudioUnit plugin support」が、macOS では何も見つけられない。違うプラグインを選んでも同じものが読み込まれる。ライブラリをアンロードできない。
- 確度: 確認済み(走査フラグとパス生成をたどった。ホスト自体がスタブであることは考慮済み)
- 修正方針: 拡張子が .vst3/.component のディレクトリを bundle として列挙し、`QDirIterator::Subdirectories` の再帰をそこで止める。uniqueId にはフルパスを含める。ダイアログを閉じるときに `unloadPlugin` を呼ぶ。

### [Low] プロジェクトファイルのエフェクトパラメータを検証していないため、Blur がハングし、Noise が未定義動作になる
- 区画: part14
- 場所: src/VideoEffect.cpp:1979(`int r = qMax(1, static_cast<int>(radius))`)、src/VideoEffect.cpp:2262-2267(`rng->bounded(2*a+1)`)、読込 src/ProjectFile.cpp:2372-2374
- 問題: `effectFromJson` は param1..3 を範囲チェックせずに読む。applyBlur は radius に上限がなく、画素ごとに ±r のループを 3 パス×2 回まわすので、radius=1e6 でフレーム 1 枚に事実上終わらない計算量になる。radius ≥ 2^31 では double→int 変換が未定義動作になる。applyNoise は amount が負のとき `bounded()` に負の値を渡し、assert またはノイズの暴走を起こす。GPU プレビューは `qBound(0,…,8)` で丸めるので、プレビューでは気づけず、書き出し時に固まる。
- 発生条件: 手で編集した、または外部から受け取った .vsep で、Blur の param1 が巨大な値、あるいは Noise の param1 が負。
- 影響: 書き出しやサムネイル生成がハングする(DoS)。
- 確度: 確認済み
- 修正方針: `effectFromJson` で `paramSchemaFor` の min/max に丸める。applyBlur と applyNoise の中でも schema の範囲で qBound する。

### [Low] KeyframeDialog が補間種別を範囲外で配列参照する。Bezier のキーを選ぶと Linear で追加される
- 区画: part14
- 場所: src/VideoEffectDialogs.cpp:604-619(`interpNames[static_cast<int>(kf.interpolation)]`)、597。原因は src/Keyframe.cpp:29-30
- 問題: `interpolationFromJson` は `interpolation` の整数をそのまま enum に cast する。KeyframeDialog::refreshList は 9 要素の配列をその値で添字参照するので、9 以上や負の値では範囲外読み出しになり、クラッシュするか不定の文字列が出る。また、コンボに Bezier の項目がないため、Bezier のキーを選ぶと `findData` が -1 になり、そのまま「Add Keyframe」を押すと Linear で上書き追加される。
- 発生条件: interpolation が範囲外のキーフレームを含むプロジェクトで、キーフレーム編集ダイアログを開く。または Bezier のキーを選んで追加する。
- 影響: クラッシュする。ベジェカーブが黙って線形になる。
- 確度: 確認済み
- 修正方針: 読込時に enum の範囲へ丸める。表示は範囲チェックしてから行う。コンボに Bezier を加えるか、Bezier のキーを選んだときは元の補間を保つ。

### [Low] 文字起こしハイライトで、LLM が start/end を文字列("00:12.500" など)で返すと、エラーにならず 0 秒に潰れる
- 区画: part14
- 場所: src/TranscriptHighlighter.cpp:585-586(`object.value("start").toDouble()`)、プロンプト 517-520
- 問題: プロンプトでは各行を `mm:ss.mmm` 形式で示しているのに、応答は `<sec>` の数値を要求している。LLM が時刻を文字列で返すと(入力の書式を真似た場合や、"12.5" のように引用符付きの数値の場合)、`QJsonValue::toDouble()` が 0 を返す。エラーにならないまま `clampHighlightsToTranscript` で先頭に寄せられる。
- 発生条件: モデルが `"start":"01:23.000"` や `"start":"83.0"` を返す。
- 影響: すべてのハイライトが 0 秒(文字起こしの先頭)になる。それが `m_lastHighlights` として自動カットの入力に使われる。
- 確度: 可能性あり(コード上は確実に 0 になる。LLM がその形式で返す頻度は実測していない)
- 修正方針: 値が文字列なら数値と `mm:ss(.mmm)` の両方として解析する。解析できない要素は捨てて警告を出す。プロンプトの時刻も秒表記に揃える。

### [Low] インクリメンタル保存で、全角数字で終わるファイル名は番号を増やせない可能性
- 区画: part14
- 場所: src/VersionedSave.cpp:233-243
- 問題: 末尾の数字を `QChar::isDigit()`(Unicode の Nd。全角「１２」やアラビア数字も含む)で切り出し、`toLongLong` で解析している。`toLongLong` が ASCII 以外の数字を受け付けない場合、`ok=false` となって空文字列を返し、保存に失敗する。
- 発生条件: 「企画案１.vsep」のように全角数字で終わる名前で Ctrl+Alt+Shift+S を押す。
- 影響: 「Could not create an incremented project file name.」と表示されて保存できない(データ損失はない)。
- 確度: 可能性あり(Qt6 の `QString::toLongLong` が非 ASCII 数字を受け付けるかは、ビルドできず実測していない)
- 修正方針: 数字判定を `'0'..'9'` に限るか、全角数字を正規化してから解析する。

### [Low] 音声オーバー録音の開始に失敗しても、ダイアログが「録音中」の表示になる
- 区画: part15
- 場所: src/VoiceOverDialog.cpp:264-283 (onCountdownTick)、:327-331 (onRecordingFailed)。src/VoiceOverRecorder.cpp:41-69, :118-126 (失敗を同期で emit)
- 問題: m_recorder->startRecording() はディレクトリ作成・ファイル open・形式非対応・capture 開始のどこで失敗しても recordingFailed を同期で emit し、onRecordingFailed が Idle に戻して警告を出す。ところが戻った直後に、onCountdownTick が無条件で setPhase(Phase::Recording) を呼び、recordingStarted も emit する。
- 発生条件: 保存先に書けない、マイクが 48 kHz / mono / Int16 に対応しない、などで録音の開始に失敗する。
- 影響: 警告を閉じると、録音インジケータが点滅し、経過時間が進み、Stop ボタンが押せる状態になる。実際には録音しておらず、Stop は isRecording()==false なので何もしない。録音ボタンも無効のままなので、ダイアログを閉じるしかない。recordingStarted の受け手には誤った開始通知が届く。
- 確度: 確認済み (同期 emit → Idle に設定 → 戻って Recording で上書き、の順序を追跡)
- 修正方針: startRecording の後で `if (!m_recorder->isRecording()) return;` を挟んでから Recording に切り替える。

### [Low] 録音停止時に、最後に溜まっていた音声を捨てる
- 区画: part15
- 場所: src/VoiceOverRecorder.cpp:162-181 (stopRecording)、:129-131 (readyRead のガード)
- 問題: stopRecording は先に m_recording=false にしてから m_audioSource->stop() と processEvents で「残りの readyRead を処理」しようとする。しかし readyRead のハンドラは `!m_recording` で即 return するので、デバイスのバッファに残っていた末尾の PCM は書き込まれない。エラー時の経路 (:95-111) も同じ。
- 発生条件: Stop を押して録音を止める (毎回)。
- 影響: ナレーションの最後の数十 ms (バッファの周期分) が切れる。語尾が欠けることがある。
- 確度: 可能性あり (コード上は確実に捨てる。欠ける長さは QAudioSource のバッファ量で決まり、実測していない)
- 修正方針: m_recording を false にする前に、m_inputDevice->readAll() の残りを書き込む (または stop の前にフラグを分けて drain する)。

### [Low] Vimeo のジョブの状態列が「Idle」のまま変わらない (失敗しても Idle と表示される)
- 区画: part15
- 場所: src/VimeoUploadDialog.cpp:141-153 (addJob の後で行を挿入)、src/VimeoUploadManager.cpp:27-31, :108-109, :152 (addJob の中で Idle → Uploading → (同期の失敗なら) Failed を emit)
- 問題: Manager::addJob は startNextQueued を同期で呼び、jobStateChanged(Uploading) を emit する。token が空などで同期に失敗した場合は jobStateChanged(Failed) も emit する。どれもダイアログが行を挿入する前に飛ぶので、rowForJobId が -1 になって捨てられる。その後に挿入する行は固定で「Idle」と書かれる。
- 発生条件: ダイアログで Add Job する (毎回)。
- 影響: アップロード中も「Idle」と表示される。即座に失敗したジョブも「Idle」のままで、ユーザーには失敗が分からない (上の token の問題と重なると、何も起きないように見える)。
- 確度: 確認済み
- 修正方針: 行を先に挿入してから addJob を呼ぶ (jobId はあとで setData する)。または挿入後に manager->job(jobId).state で表示を合わせる。

### [Low] Vimeo のジョブを取り消しても、待機中の次のジョブが始まらない
- 区画: part15
- 場所: src/VimeoUploadManager.cpp:34-52 (cancelJob)
- 問題: onClientFinished / onClientFailed は最後に startNextQueued() を呼ぶのに、cancelJob は呼ばない。取り消したジョブ (Failed になる) の枠が空いても、Idle のジョブが起動しない。
- 発生条件: 4 件以上 (同時実行の上限 3 を超える数) を積み、アップロード中の 1 件を Cancel Job する。
- 影響: 待機中のジョブは、別のジョブが完了・失敗するか、新しく Add Job するまで始まらない。
- 確度: 確認済み
- 修正方針: cancelJob の最後に startNextQueued() を呼ぶ。

### [Low] プレビューの全体ミュートがトラックごとのミュート状態を上書きし、その状態が undo のスナップショットに入る
- 区画: part15
- 場所: src/VideoPlayer.cpp:2306-2315 (setMuted が A1..A16 の setTrackMute をまとめて呼ぶ)、src/AudioMixer.cpp:2210-2221 (collectTrackState に mute を含む)、src/MainWindow.cpp:9646-9690 (undo の外部状態に mixer の状態を入れて復元する)
- 問題: 一時的な「プレビューの全体ミュート」を、undo で保存・復元されるトラックごとの muted フラグに直接書き込んでいる。全体ミュートの状態 m_muted は undo の対象外なので、二つがずれる。
- 発生条件: 全体ミュート中に undo 可能な操作をし、ミュートを解除してから undo する (またはその逆)。音声オーバーのダイアログを閉じた後の setMuted(wasMuted) でも同じ書き換えが起きる。
- 影響: undo のあと、UI はミュートしていないのに全トラックが無音になる (またはその逆)。全体ミュートを 2 回押し直すまで直らない。
- 確度: 可能性あり (経路はコードで確認した。UI 上のミュートボタンの位置と操作手順は実機で確認していない)
- 修正方針: 全体ミュートはミキサーのマスターゲイン (または専用のフラグ) で実装し、トラックごとの muted には触らない。

### [Low] yt-dlp の URL 判定が先頭に固定されておらず、URL を位置引数のまま渡すので、オプションを注入できる
- 区画: part16
- 場所: src/YtdlpDownloader.cpp:10-11 / :64-75
- 問題: `kYoutubeRegex` に `^` が無いので、文字列のどこかに `https://youtube.com/` があれば通る。さらに `url` を `--` なしで argv の最後に置いている。`--exec=calc #https://youtube.com/` のような文字列は 1 つの引数として yt-dlp に渡り、オプションとして解釈される(`--exec` は任意コマンドを実行する)。`https://evil.example/?u=https://youtube.com/` のような URL で、YouTube 以外のサイトからもダウンロードできる。
- 発生条件: 細工した「URL」をユーザーが貼り付ける(SNS などで配られた文字列)。
- 影響: 任意コマンドの実行、または意図しないサイトからのダウンロード。
- 確度: 確認済み(コード上。発生にはユーザーの貼り付けが必要)
- 修正方針: 正規表現を `^https?://(www\.|m\.)?(youtube\.com|youtu\.be)/` に固定し、`QUrl` で host を検証する。args の URL の前に `--` を入れる。

### [Low] YouTube アップロードの一時停止 / キャンセルが送信中の HTTP リクエストを中止しない。再開すると 0 バイトから送り直す
- 区画: part16
- 場所: src/YoutubeUploadManager.cpp:77-151 / :199-202, src/YoutubeUploadClient.cpp:125-129 / :196-199
- 問題: `pause()` / `cancel()` は `m_currentJobId` を消して次のジョブへ進むだけで、`upload::Client` の送信中の reply を abort しない(中止する API が無い)。最後のチャンクを送信中にキャンセルすると、サーバ側ではアップロードが完了して動画ができる(UI は「cancelled」と表示する)。新しい initiate / uploadChunk は、古い reply を `deleteLater` するだけで finished の接続を切らない。そのため古い reply が先に終わると、`onXxxReplyFinished` が `m_xxxReply`(新しいジョブの、まだ終わっていない reply)を処理して status 0 のエラーにする可能性がある。resume は `processNext()` で uploadedBytes を 0 にして新しいセッションを作るので、コメントにある「queryStatus から再開」はされていない。
- 発生条件: アップロード中に Cancel / Pause を押す。
- 影響: キャンセルした動画が YouTube に作られる。再開すると全体を送り直す。次のジョブが誤って失敗する可能性がある。
- 確度: 確認済み(キャンセル後に送信が続く点)。次ジョブの誤失敗は可能性あり(deleteLater と finished の順序による)。
- 修正方針: Client に `abort()` を追加し、reply->abort() と disconnect を行う。pause / cancel から呼ぶ。resume では sessionUri を残して `queryStatus` から再開する。

### [Low] OAuth のコールバック: 8080 番が使用中だと token 交換の redirect_uri が認可要求と食い違う。code を含まない最初の接続で認可フローが終わる
- 区画: part16
- 場所: src/YoutubeOAuth.cpp:110-117 / :175-184 / :241-244 / :168-173
- 問題: `handleCallbackRequest` は `stopCallbackServer()`(`m_callbackPort = 0` にする)を呼んでから `exchangeCodeForTokens()` を呼ぶ。exchange 側は `m_callbackPort != 0` のときだけ port を差し替えるので、フォールバックのランダムポートで認可したときは redirect_uri が 8080 に戻り、Google が `redirect_uri_mismatch` を返す。パスも確認しておらず、code の無いリクエスト(先読みや別のローカルアクセス)が 1 回来ただけで authError を出してサーバを止める。
- 発生条件: 8080 番を他のプロセスが使っている。または認可前にローカルからアクセスがある。現状は上の指摘のとおり UI からこのフローを呼ぶ経路が無いので、まだ表に出ていない。
- 影響: サインインが失敗する。
- 確度: 確認済み
- 修正方針: 認可要求で使った redirect URI をメンバに保存し、exchange でもそれを使う。path が `/callback` でないリクエストは 404 を返して無視する。

### [Low] ラウドネス測定で動画ファイルを選ぶと、映像の圧縮パケットをファイル全体ぶんメモリに溜める
- 区画: part16
- 場所: src/LoudnessMaster.cpp:224-225 / :287-289, src/libavcore/Decode.cpp:338-349
- 問題: `decodeAudioWithMediaDecoder` は `open(path, true)` のあと `nextAudioFrame()` だけを呼ぶ。MediaDecoder は映像を必須で開き、音声を読む途中で出てきた映像パケットを `m_videoPackets` に複製して溜める(取り出されない)。ヘッダ自身が「交互に消費すること」を前提にしている。
- 発生条件: ラウドネスマスターダイアログで「All Files」から動画を選ぶ(mp4 / mov など)。
- 影響: 数 GB の動画では、圧縮映像がまるごと RAM に載り、メモリ不足になる。GUI スレッドで動くのでその間は固まる。
- 確度: 確認済み
- 修正方針: 音声だけを読むモード(映像パケットを捨てるフラグ)を MediaDecoder に追加する。または LoudnessMaster では最初から decodeAudioWithLibav を使う。

### [Low] VideoFilterGraph(手ぶれ補正)が映像以外のストリームをすべてストリームコピーするため、データトラックやアタッチメントを含む素材ではヘッダ書き込みで失敗する
- 区画: part16
- 場所: src/libavcore/VideoFilterGraph.cpp:799-824
- 問題: `copyAudio` が true だと、最初の映像以外のストリームを種類に関係なく(data / attachment / 2 本目の映像を含めて)出力に追加する。iPhone の MOV に入っている mebx や tmcd などのデータトラック、mkv のフォントの attachment は、mp4 muxer が「Could not find tag for codec」で `avformat_write_header` を失敗させる。
- 発生条件: 手ぶれ補正(Translation / deshake)を、データトラックを持つ素材にかける。
- 影響: 手ぶれ補正全体が失敗する。
- 確度: 可能性あり(出力の拡張子と素材の構成による。コード上の挙動は確認済み)
- 修正方針: コピーするのは AVMEDIA_TYPE_AUDIO だけにする(または `avformat_query_codec` で出力側が対応しているか確認する)。

### [Low] HDR ODT 経路(VEDITOR_HDR_ODT)で、PQ / HLG のクリップがリニア化されないまま「リニア Rec.2020」として合成・トーンマップされる
- 区画: part16
- 場所: src/color/ClipColorTransform.cpp:206-207, src/TimelineFrameRenderer.cpp:3141-3171, src/color/ClipOdt.cpp:71-92
- 問題: `toLinearWorking` は PQ / HLG を「そのまま返す」(コメントで未対応と明記)。ODT 経路ではその結果を他クリップのリニア値と一緒に合成し、`applyOdt16` がリニア入力として ACES トーンマップと OETF を掛ける。PQ 符号値をリニアとして扱うので、画が大きく白っぽくなる。
- 発生条件: 環境変数 VEDITOR_HDR_ODT を有効にして、PQ / HLG 素材を含むタイムラインを描画する。
- 影響: HDR ODT の主な対象である HDR 素材の画が壊れる。
- 確度: 確認済み(環境変数で有効にしたときだけ)
- 修正方針: PQ / HLG の EOTF を実装するまでは、ODT 経路で PQ / HLG 素材を検出したら ODT を無効にしてフォールバックする。

### [Low] チャプターの最初の開始時刻が 0 秒より大きく 0.5 秒未満だと「0:00」が 2 行になり、YouTube がチャプターとして認識しない
- 区画: part16
- 場所: src/YoutubeChapterGen.cpp:13 / :53-62
- 問題: イントロを入れるかどうかを `startSec > 0.0` で判定し、時刻の表示には `qRound` を使う。0.3 秒なら「0:00 イントロ」と「0:00 タイトル」が並ぶ。1 秒未満の間隔で並ぶ候補も同じ時刻の重複になる。
- 発生条件: ハイライト検出の最初の候補が 0.5 秒未満。
- 影響: 概要欄に貼ってもチャプターが無効になる。
- 確度: 確認済み
- 修正方針: 丸めた後の秒で判定し、同じ秒(か YouTube の最小間隔の 10 秒未満)のものはまとめる。

### [Low] Windows で、ファイルを関連付けや「プログラムから開く」で渡すと、ANSI コードページに無い文字を含むパスが壊れる
- 区画: part16
- 場所: src/main.cpp:1610-1611(ほかに :605 / :626)
- 問題: `QString::fromLocal8Bit(argv[1])` を使っている。Windows の argv は ANSI コードページ(日本語環境なら CP932)で渡されるので、CP932 に無い文字(é などのラテン文字、絵文字、簡体字など)は "?" に置き換わる。英語環境の Windows では日本語のパスがすべて壊れる。`--hdr-probe=<file>` も同じ。
- 発生条件: 上記の文字を含むパスをコマンドライン引数で渡す。
- 影響: 起動時にファイルを開けない。
- 確度: 確認済み
- 修正方針: QApplication を作った後は `app.arguments()`(Windows では GetCommandLineW)を使う。QApplication より前に処理する引数は `CommandLineToArgvW` で取る。

### [Low] 楕円 / 矩形マスクのベジェ情報(接線)が、一度保存して読み込むと失われ、64 頂点の折れ線になる
- 区画: part16
- 場所: src/mask/ClipMask.cpp:462-485 / :490-506、比較対象 src/MainWindow.cpp:1170-1188
- 問題: 保存時の `fromMaskSystem` は、楕円を接線付きの 4 点ベジェとして JSON に書く。読み込み時の `toMaskSystem` はそれを 16 分割で平坦化して `MaskShape::Path` にし、名前にベジェのメタデータ(`#VEDITOR_MASK_BEZIER:`)を付けない。そのため次に編集するとき、`editorPointsFromMask` が 64 個前後の角の点を返し、次の保存でも接線の無い折れ線として保存される。
- 発生条件: GUI で作った楕円 / 矩形マスク(メタデータ無し)を含むプロジェクトを保存して開き直す。
- 影響: マスクの編集ハンドルが 4 点のベジェから多数の頂点に変わる(見た目の形はほぼ同じ)。
- 確度: 確認済み
- 修正方針: `toMaskSystem` で、接線を持つマスクのときは名前にベジェのメタデータを入れる。または Mask にベジェ点のフィールドを追加する。

### [Low] X(Twitter)投稿で、280 文字の判定が UTF-16 の単位で行われ、media_type も video/mp4 に固定されている
- 区画: part16
- 場所: src/XVideoDialog.cpp:100, src/XVideoUpload.cpp:88
- 問題: `tweetText.length() > 280` は UTF-16 のコード単位を数える。X は CJK を 2 として数える(重み付き)ので、日本語が 141〜280 文字の投稿はローカルの判定を通り、動画を全部アップロードした後の CREATE TWEET で拒否される。ダイアログでは .mov を選べるのに、INIT の media_type は常に video/mp4 になる。
- 発生条件: 日本語が 141 文字以上の本文。または .mov ファイル。
- 影響: アップロードが終わってから投稿に失敗する(時間と通信量が無駄になる)。
- 確度: 確認済み(X の文字数の数え方は X の公開仕様による)
- 修正方針: twitter-text と同じ重み付きで数える。media_type は拡張子か probe の結果から決める。

### [Low] `grabFrameAt` が EOF でデコーダを flush しないため、末尾付近の時刻では数フレーム前の画像を返す
- 区画: part16
- 場所: src/libavcore/FrameGrab.cpp:161-191
- 問題: `av_read_frame` が EOF になるとそのままループを抜け、`avcodec_send_packet(nullptr)` を送らない。B フレームやフレームスレッドのぶんデコーダの中に残っている末尾の数フレームを取り出せず、`result` には最後に変換した(目標より前の)フレームが残る。
- 発生条件: クリップの out 点や最終フレームの付近を取得する(サムネイル、フレーム取得系のツール)。
- 影響: 末尾付近で、表示と違うフレームが返る。
- 確度: 確認済み
- 修正方針: EOF のあと NULL パケットを送り、`receive_frame` で残りを取り出してから判定する。

### [Low] MCP HTTP サーバで、1 回応答した後の keep-alive 接続がアイドルタイムアウトの対象から外れ、接続枠（16）を占有し続ける
- 区画: part17
- 場所: src/mcp/McpHttpServer.cpp:453-455（`request = Request{}` で `lastReceivedMs` が 0 に戻る）、:497-499（`onIdleTimeout` は `lastReceivedMs > 0` のときだけ切断する）、:298（`kMaxConnections` を超えると 503）
- 問題: 1 件目のリクエストを処理した後、次のデータが届かない接続は `lastReceivedMs == 0` のまま `m_pending` に残る。30 秒のアイドル切断（`kIdleTimeoutMs`）は、最初のリクエストを送る前の接続にしか効かない。
- 発生条件: クライアントが接続プール（QNetworkAccessManager は 1 ホストあたり最大 6 本。stdio ブリッジもこれを使う）を開いたままにする。または、ローカルのプロセスが認証不要の 404 リクエストを 1 回ずつ送った接続を 16 本保持する。
- 影響: 合計 16 本に達すると、新しい MCP クライアントの接続はすべて 503 になる。サーバを再起動するか、クライアントが切断するまで回復しない。
- 確度: 確認済み（状態遷移をコード上で追跡した。実行はしていない）
- 修正方針: 応答後に再登録するとき `lastReceivedMs` を現在時刻にする（keep-alive のアイドル時間にも `kIdleTimeoutMs` を適用する）。

### [Low] クラッシュ時に完全メモリダンプ（MiniDumpWithFullMemory）をログディレクトリへ書き、削除もローテーションもしない
- 区画: part17
- 場所: src/util/CrashHandler.cpp:243-273（`MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo`）、:354-361（`~/.veditor/logs/veditor_crash_*.dmp`）。リポジトリ内にこれを片付けるコードはない（grep で確認）。
- 問題:
  - 動画編集中のプロセスのメモリ全体（フレームキャッシュ 256MiB、デコーダのバッファなど）をクラッシュのたびに丸ごと書き出す。
  - ダンプには、メモリ上にある MCP の Bearer トークンや AI 連携の API キー、プロジェクトの内容がそのまま入る。
  - ログ（`veditor_*.log`）も起動のたびに新しいファイルを作り、消さない。
- 発生条件: Windows 版でクラッシュが繰り返される場合。
- 影響: 1 回あたり数 GB のダンプがホームディレクトリにたまり、ディスクを圧迫する。ダンプを不具合報告に添付すると、トークンなどの秘密も渡る。
- 確度: 可能性あり（ダンプの種類と、片付ける処理がないことは確認済み。実際のサイズは測っていない）
- 修正方針: 既定は `MiniDumpNormal | MiniDumpWithThreadInfo`（HangDump.cpp と同じ）にし、完全ダンプは環境変数でオプトインにする。古いダンプとログは N 件に保つ。

### [Low] 一時停止中に 1 フレーム送り (stepForward) すると、テキストオーバーレイ・FadeIn/Out・重なりトランジション・GL のクリップグレードが 1 つ前のフレームの時刻で評価される
- 区画: part18
- 場所: src/VideoPlayer.cpp:3236-3243 (stepForward の同一エントリ経路で decodeNextFrame(true))、:5586-5605 (decodeNextFrame は新しいフレームの displayTimelineUs を渡して表示し、その後で updatePositionUi が m_timelinePositionUs を更新する)、:3552-3604 (displaySeekFrameConformed)、:3815-3822 (displayFrame の T = m_timelinePositionUs)、:4176-4177 (composeFrameWithOverlays の previewTimelineUsec = m_timelinePositionUs)、:3618-3631 (pushActiveClipColorCorrectionToGlPreview)、:3929-3935 (スタビライザの時刻)
- 問題: displayFrame は引数 displayTimelineUsec を受け取るが、それを使うのは special composite と still compare だけ。テキストの焼き込み (bakeTextOverlaysForDisplay)、プレビュー FX、エッジフェード (applyEdgeFadeStep)、重なりトランジションの判定と合成、GL へのキーフレーム付きグレード、スタビライザの時刻は、まだ更新されていない m_timelinePositionUs (1 つ前の再生ヘッド) で評価している。stepForward はデコード→表示→updatePositionUi→forceTimelineUiToCurrent の順で、その後に再描画しない (positionChanged の受け手 pushAnimatedHslPreview / updateBrushPreviewProgress / refreshSolvePointOverlay も再描画しない)。
- 発生条件: シーケンスモードで一時停止中に「1F→」ボタンで、同じクリップの中を 1 フレーム進める。テキストの開始・終了フレーム、FadeIn/FadeOut、Wipe などの重なり区間、グレードのキーフレームがある位置。
- 影響: テロップの出始めが 1 フレーム遅れて見える (終わりも 1 フレーム遅れる)。フェードの濃さも 1 フレーム分ずれる。書き出しと「現在フレームを書き出し」は正しい時刻なので、フレーム送りでタイミングを合わせると 1 フレームずれる。1F← は seekToTimelineUs が先に m_timelinePositionUs を更新するので正しく、前後で表示が食い違う。
- 確度: 確認済み (stepForward → decodeNextFrame → displaySeekFrameConformed → displayFrame → composeFrameWithOverlays の順で、m_timelinePositionUs の更新が表示より後であることを追跡した。実機での見え方は確認していない)
- 修正方針: displayFrame / composeFrameWithOverlays / presentDisplay / pushActiveClipColorCorrectionToGlPreview の時刻をすべて、解決済みの timelineUsec 引数に揃える。あるいは decodeNextFrame で表示の前に m_timelinePositionUs を投影値に更新する。

### [Low] 多トラックプレビューの CPU 合成で、ブレンドモード付きレイヤーの順序と V1 の扱いが書き出しと違う
- 区画: part18
- 場所: src/VideoPlayer.cpp:7894-7922 (composeMultiTrackFrameInto)、:7797-7825 (composeMultiTrackFrame)、:1195 (applyLayerMotionOpacity が entry.blendMode をそのまま入れる)、:6844 (layers は sourceTrack 降順に並べ替え済み)。対照: src/TimelineFrameRenderer.cpp:3205-3218 (特殊ブレンドは overlays を V2→Vn の昇順で適用)、:2644 (V1 は常に Normal)、:2744-2745 (V2+ も isVfxFootage のときだけ blendMode を使う)
- 問題:
  - プレビューの 2 パス目 (特殊ブレンド) は、paint order (sourceTrack 降順: Vn→V2) の配列をそのまま走査する。書き出しは V2→Vn の順に blendImages を掛ける。Overlay / SoftLight / HardLight / ColorDodge などは順序で結果が変わる。
  - プレビューは V1 にも entry.blendMode を使う。V1 の blendMode が Normal でないと、V1 は 1 パス目で描かれず、2 パス目で V2+ の上にブレンドされる。書き出しでは V1 は常に Normal で一番上に不透明で描かれる。isVfxFootage でないクリップの blendMode も、プレビューは使うが書き出しは無視する。
  - 隣の composePreviewFrameWithAdjustmentClips (:858-859) は昇順で書き出しと揃っており、この 2 関数だけ順序が違う。
- 発生条件: (1) エフェクトライブラリの VFX 素材を 2 本以上同時に重ね、少なくとも一方のブレンドモードを Screen / Add 以外 (Overlay など) にする。(2) VFX 素材のクリップを V1 に移す、または手で編集したプロジェクトで通常クリップに blendMode を書く。
- 影響: プレビューと書き出しで合成結果が違う。(2) の場合、プレビューでは V2 が透けて見えるのに、書き出しでは V1 が不透明に覆う。
- 確度: 可能性あり ((1) の順序の違いはコードで確定。既定の Screen / Add どうしなら可換なので差は出ない。(2) は VFX 素材を V1 に移せるかを UI で確かめていない)
- 修正方針: 特殊ブレンドのパスは sourceTrack 昇順で走査する (composePreviewFrameWithAdjustmentClips と同じ)。applyLayerMotionOpacity で、sourceTrack==0 または !isVfxFootage のときは blendMode を Normal にする。

### [Low] カーブ / HSL セカンダリは「シグナルで来たクリップ」を表示し、「選択のある最初の映像トラックのクリップ」に書くため、複数選択時に別のクリップを上書きする
- 区画: part19
- 場所: src/ColorGradingPanel.cpp:600-606, 712-724 (HSL の復元), 1041-1062, 1064-1077 (カーブの復元と書き込み), 95-114 (selectedClipIndex), 164-193 / 195-220 (書き込み)
- 問題: 復元は `Timeline::clipSelectedOnTrack(trackIdx, clipIdx)` で受けたクリップから行います。一方、書き込みの `writeCurvesToSelectedClip` / `writeHslSecondaryToSelectedClip` は `selectedClipIndex()` で V1 から順に探し、最初に選択のあるトラックの `selectedClip()` に書きます。Ctrl+クリックの追加選択や、リンクグループによる選択の伝播 (Timeline.cpp:12936-12952) では、下位トラックの選択が残ったまま上位トラックのクリップが clipSelectedOnTrack で通知されます。
- 発生条件: V1 のクリップを選んだ状態で V2 のクリップを Ctrl+クリックする (または V1 と V2 にまたがるリンクグループの V2 側をクリックする)。その後、カーブの点か HSL のスライダーを動かす。
- 影響: パネルには V2 のカーブ / HSL が出ているのに、V2 の値を土台にした編集結果が V1 のクリップに書かれます。V1 のグレードが黙って置き換わり、V2 は変わりません。
- 確度: 確認済み (追加選択では clearOther が走らないこと: Timeline.cpp:12926、emit clipSelectedOnTrack(videoTrackIdx, index): 12965、selectedClipIndex が先頭から探すことを確認)
- 修正方針: 復元に使った (trackIdx, clipIdx) を覚えておき、書き込みもそのクリップに対して行う。または、MainWindow の selectedVideoClipRef と同じ規則に統一する。

---

## 優先して直すべき上位 5 件(全体)
1. **操作対象の取り違え(V1 固定 API)の一掃**: エフェクトコントロール(第 2 部 High)、カラーグレーディングパネル(同)、速度・音量・パン・トランジションの A1 同番号書き換え(Medium)。単一引数の `setClipEffects` / `setClipKeyframes` / `setClipColorCorrection` を廃止し、(トラック, クリップ) 指定版に統一する。編集が黙って失われるかデータを上書きするため最優先。
2. **MCP/AI チャットからの任意パス書き込み**(第 1 部 High)と **式パーサの再帰上限**(第 1 部 High)。外部入力でファイル破壊・クラッシュが起きるため。
3. **保存に載らない状態**: マーカー(第 2 部 High)、スタビライズ結果、非アクティブシーケンスのタイムリマップ(第 1 部)、シーケンス切替で sidecar が混線(第 1 部)、未保存確認なし(第 1 部)。
4. **クラッシュ系**: ラムダ + `Qt::UniqueConnection` による 2 回目実行時の解放済みダイアログ参照(トラッキング/スタビライズ/字幕生成など)、1D/サイズ 1 の .cube LUT の範囲外読み、Vimeo キャンセル、書き出しスレッドのライブ Timeline 無同期読み取り。
5. **書き出し結果の誤り**: V1 に隙間があると書き出しが失敗、24/48/12fps のフレーム時刻切り捨て(第 1 部)、ビットレート指定が crf で無視される、マルチカム同期のオフセット誤り、カラーパネル/VFX コントロールがプレビュー専用。

## カバレッジ
- 精読: 19 区画(part01〜17 が本体全体、part18/19 が読み残しの補完)。割当 828 範囲のうち FULL 819。PARTIAL 9 件中 7 件は part18/19 で全行読み直し済み。
- 残る未精読: `src/ThemeManager.cpp` の QSS 文字列の各スタイル値(ロジックなし。構造のみ確認)、`CMakeLists.txt` のソース一覧(重複・存在しないファイルの機械チェックのみ)。
- 区画とファイル範囲の対応(先頭〜末尾ファイル): part01 AIHighlight〜AudioRestorationDialog / part02 AudioTrackFx〜ColorGradingPanel.cpp / part03 ColorGradingPanel.h〜EqualizerPanel / part04 ExportDialog〜HueVsSatEditor / part05 ImportHubDialog〜MainWindow.cpp 1-7460 / part06 MainWindow.cpp 7461-〜MarkerPanelDock / part07 MaskSystem〜ObjectRemoval / part08 ObjectRemovalDialog〜ProjectFile.cpp / part09 ProjectFile.h〜SequenceSettingsDialog / part10 ShaderEffect〜TextAnimPresets / part11 TextAnimator〜TimecodeBurnInDialog + scripts/・*.bat・CMakeLists.txt / part12 Timeline.cpp 1-8701 / part13 Timeline.cpp 8702-〜TrackMatteKey / part14 TrackerLink〜VideoEffectDialogs / part15 VideoPlayer〜WhisperTranscribeDialog / part16 WhisperTranscriber〜mcp/McpConnectionInfoDialog / part17 mcp/McpEditorTools〜util / part18 VideoPlayer.cpp・GpuLayerCompositor.cpp の補完 / part19 MainWindow.cpp 3530-7460・main.cpp・Timeline.h・ColorGradingPanel.cpp・AudioMixer.cpp の補完。

## 調べられなかった範囲とその理由
- 実行による再現: ビルドが通らない(下記)ため、どの指摘も実機・selftest では確認していません。「確認済み」はコード上で呼び出し元から呼び出し先まで追跡したという意味です。
- `src/selftests/`(約 11.7 万行)はテストコードのため精読対象外とし、cppcheck の機械走査のみ行いました。ただし本番の認証情報を壊す selftest(第 2 部 Medium)は本体側の区画で検出しています。
- 外部仕様に依存する点(FFmpeg の qsin 曲線・libx264 で crf が bitrate より優先される挙動・Final Cut Pro / Resolve の XML 解釈・各 SNS API の上限)は知識に基づく判断で、この環境では一次資料を確認していません。確認方法: FFmpeg の `libavfilter/af_afade.c` と `libavcodec/libx264.c` のソース、各ベンダーの XML / API リファレンス。
- Windows / macOS 固有の動作(日本語パスの ANSI 変換、Defender 除外、VST bundle)は Linux 上での静的読解のみ。
- リポジトリに `AGENTS.md` と `graphify-out/` が無いため、CLAUDE.md の graphify 参照はできませんでした。
- 残高・消費額: この環境から確認する手段がありません。今回の全体監査で使ったサブエージェントのトークン数は合計約 724 万です(各エージェントの完了通知の実測値の合計。第 1 部の 3 エージェント約 69 万は含まない。料金換算は Anthropic Console の Usage で要確認)。

## 実行したビルド・テストのコマンドと結果
- 依存導入: `apt-get install qt6-base-dev qt6-multimedia-dev libqt6opengl6-dev libavformat-dev libavcodec-dev libavfilter-dev libswscale-dev libswresample-dev cppcheck` → 成功(Ubuntu noble: Qt 6.4.2、FFmpeg 6.1、cppcheck 2.13)
- 構成: `cmake -B /home/user/build18 -S . -DCMAKE_BUILD_TYPE=Release` → 成功
- ビルド: `cmake --build /home/user/build18 -j -- -k` → 失敗(rc=2、46 翻訳単位がエラー、リンク未到達)。原因はいずれも環境のライブラリが古いこと(FFmpeg 7.1+ の `avcodec_get_supported_config` 系 API 不在、Qt 6.4 の QPointer/QWeakPointer API 非互換、QtSvg 未導入)。残りの翻訳単位はコンパイル成功。
- selftest(`--selftest=all` 等): 実行ファイルを作れず未実行。
- 静的解析: `cppcheck -j4 --library=qt --enable=warning,performance,portability --inconclusive --std=c++17 -Isrc src` → **走査は未完了(この報告の時点で実行中)**。途中までの出力に含まれていた本体の warning 以上 21 件を手で確認した結果、実害のある新規不具合はなし(`TextManager::defaultTemplate` の `outline2Width` 未初期化は呼び出し元が無く到達しない、`Timeline::insertAudioClipAtPlayhead` の負インデックスは呼び出し元が常に 2 を渡す、`TextOverlayBake.cpp:288` / `McpEditorTools.cpp:1246` / `CollabProjectShare.cpp:52` は誤検出)。初回の `--library=qt` なしの走査は Qt マクロを解釈できず有効な結果になりませんでした。
- 補助計算: 24/48/12fps のフレーム時刻切り捨て件数を Python で算出(第 1 部に記載)。
