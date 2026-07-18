# Sprint 17-22 スタブ監査 (STUB_AUDIT.md)

**目的**: Sprint 17-22 で追加されたモジュールについて、コアロジックが実装済みか、スタブにとどまるかを正直に評価する。  
**生成日**: 2026-05-16  
**調査方法**: 各 `.cpp` を `qWarning / stub / dummy / fallback / no-op / TODO` でgrepし、コアロジック（avformat呼び出し、数学、XMLシリアライズ等）の実在を目視確認。

---

## モジュール評価表

| Module | 区分 | 根拠 (file:line または "core logic real") | push可否への影響 |
|---|---|---|---|
| YoutubeOAuth | PARTIAL | `YoutubeOAuth.cpp:28` — `clientId/clientSecret` はデフォルトで空文字列。OAuth フロー全体（認可URL生成、コールバックサーバー、トークン交換、リフレッシュ）は実装済みだが、実動には GCP client_id/secret の注入が必須 | GCP 認証情報なしでは OAuth フロー開始後に即失敗 |
| YoutubeUploadClient | PARTIAL | `YoutubeUploadClient.cpp:226` — resumable upload (セッション開始→チャンク送信→offset取得) は完全実装。`fallback` はオフセット取得失敗時の計算上のみで機能上問題なし。認証情報依存 | 認証済みトークンがあれば実際にアップロードする |
| YoutubeUploadManager | PARTIAL | 実装済み。キュー管理・進捗通知ロジックはリアル。YoutubeOAuth/UploadClient に依存するため同条件 | 同上 |
| VimeoOAuth | PARTIAL | `VimeoOAuth.cpp:30-33,71,74` — `qWarning` 付きで `dummy-vimeo-client-id` / `dummy-vimeo-client-secret` にフォールバック。`VimeoOAuth.cpp:107` — grant 未指定時も `client_credentials` にフォールバック。OAuth フロー自体は実装済み | dummy credential のまま実動させると Vimeo API が 401 を返す |
| VimeoUploadClient | PARTIAL | TUS プロトコル (createUpload→PATCH チャンク→offset 管理) は完全実装。`VimeoUploadClient.cpp:82` — アクセストークン空チェックあり | アクセストークンなしでは即 `uploadFailed` を emit |
| VimeoUploadManager | PARTIAL | キュー管理実装済み。VimeoOAuth/UploadClient に依存 | 同上 |
| XVideoUpload | PARTIAL | `XVideoUpload.cpp:45` — "`X bearer token not set; upload will be a no-op`" の qWarning あり。`XVideoUpload.cpp:61` — bearer token 未設定で即 `uploadFailed`。ただし INIT→APPEND→FINALIZE の Twitter Media Upload API シーケンスは実装済み | bearer token なしで呼ぶと即失敗。クレデンシャルがあれば動作する可能性あり |
| InstagramPublish | PARTIAL | `InstagramPublish.cpp:31` — access token 未設定時に qWarning。Graph API 3フェーズ (container作成→ポーリング→publish) は実装済み | access token + ig_user_id なしでは動作しない |
| CloudRenderClient | PARTIAL | AWS Batch / GCP Cloud Run / 汎用エンドポイントへの submit/poll/cancel は実装済み。`CloudRenderClient.cpp:102-107` の `fallback` はユーティリティ関数の値不在時のデフォルト値のみ（機能上問題なし）。ただし `endpointUrl` 等は設定が必要 | エンドポイント未設定なら QUrl が不正になりネットワーク呼び出しが失敗する |
| FrameIoImporter | PARTIAL | `FrameIoImporter.cpp:27` — Frame.io REST API (`/assets/{id}/comments`) へのGETと応答パースは実装済み。認証ヘッダーは `config.authToken` から注入。Frame.io API キーなしでは 401 | Frame.io API キーがあれば実動する。コメント取得のみ対応（アセットアップロードは対象外） |
| SmartEditAssistant | REAL | `SmartEditAssistant.cpp:166,170` — `AutoEdit::detectSilenceFromFile` と `AutoEdit::detectSceneChanges` を呼び出し。`AutoEdit.cpp:5-6` — libavformat/libavcodec を直接使用し、avformat_open_input→av_read_frame→avcodec_receive_frame で実デコードしてシーン変化を検出 | スタブなし。libavformat リンクが必須だが、それは既存ビルドで満たされている |
| ColorMatchAnalyzer | REAL | `ColorMatchAnalyzer.cpp:82,121,143,155` — avformat_open_input→avcodec_open2→av_read_frame→avcodec_receive_frame の実デコードパス。sws_scale で RGB24 変換後に画素平均・標準偏差を計算 | 実動する。デコード失敗時は qWarning を出し空の ColorStats を返す（クラッシュしない） |
| ColorMatchLutGenerator | REAL | `ColorMatchLutGenerator.cpp:36-56` — mean/stddev 転送によるRGB空間の 3D LUT 生成ロジックが実装済み。`.cube` ファイルエクスポートも実装 | 完全スタンドアローン動作。外部依存なし |
| SubtitleTranslator | PARTIAL | `SubtitleTranslator.cpp:50-57` — Stub / GoogleV2 / DeepL の 3 プロバイダを実装。Stub プロバイダはオフラインで `"[lang] text"` を返す。GoogleV2/DeepL は API キーが空なら即 `translateFailed` を emit (`SubtitleTranslator.cpp:88,156`) | Stub は無条件で動作する。Google/DeepL は API キーがなければ動かない |
| AudioRestoration | PARTIAL | `AudioRestoration.h:35,40,47,51` — `deClick / deHum / spectralNoiseGate / processAll` はすべてインメモリ `QVector<float>` を処理するDSP関数として完全実装。**ただしファイルI/Oエントリポイントが存在しない**: `.cpp` には avformat/QFile による読み書き経路がなく、呼び出し側がサンプルを用意する必要がある | DSPアルゴリズムは実動する。ファイルを渡して処理済みファイルを得る、という使い方は現在不完全（呼び出し側でデコード/エンコードを担う必要あり） |
| AnimatedExport | PARTIAL | GIF: `AnimatedExport.cpp:239` — 独自LZWエンコーダを実装、GIF89a バイナリを直接生成。WebP: `AnimatedExport.cpp:456,480,483` — Qt の imageformats プラグイン依存で、アニメーションWebPは Qt ビルド次第で単一フレームへフォールバック (`qWarning` あり)。APNG: QImageWriter 経由 | GIF は常に動作する。WebP アニメーションはQtプラグインがなければ1フレームのみになる |
| ChromaKeyRefine | REAL | `ChromaKeyRefine.cpp:24-39` — HSV距離ベースのアルファ計算、`ChromaKeyRefine.cpp:59-86` — スピルサプレッション（Green/Blue/Red キー対応）を実装。QImage 画素処理ループが実装済み | 完全スタンドアローン動作 |
| HdrGrading | REAL | `HdrGrading.cpp:18-59` — SMPTE ST 2084 (PQ) / ARIB STD-B67 (HLG) のEOTF数式を実装。トーンマッピングとガンマ変換が実装済み | 完全スタンドアローン動作 |
| LowerThirdTemplates | REAL | `LowerThirdTemplates.cpp:73-210` — QPainter でタイトルバー、テキスト、アニメーション付き `renderFrame()` を実装。3スタイル (bar/side/wipe) のレンダリングロジックが実装済み | 完全スタンドアローン動作 |
| WatermarkOverlay | REAL | `WatermarkOverlay.cpp:16-79` — QPainter + setOpacity による透明度合成、9点アンカー配置、スケーリングを実装。テキスト/画像両モード対応 | 完全スタンドアローン動作 |
| MultiCamSync | PARTIAL | `MultiCamSync.cpp:24-56` — 正規化クロス相関によるオフセット推定ロジックは実装済み。`WaveformGenerator::generate` は libavformat/libavcodec で実デコードするため音声ファイルに対して動作する。ただし空エンベロープ時はオフセット0にフォールバック | WaveformGenerator のデコードが成功すれば同期計算は実動する |
| LoudnessMaster | PARTIAL | `LoudnessMaster.cpp:34-112` — ITU-R BS.1770-4 K重み付けフィルタ + ゲーテッド積分ラウドネス測定を実装。`LoudnessMaster.cpp:163-175` — `.raw`/`.pcm` ファイルのみ読み込みパスが存在し、48kHz モノラルを仮定。`LoudnessMaster.cpp:183-187` — それ以外の拡張子は "`audio decoding not wired`" と qWarning を出し **-23.0 LUFS を返す** | `.raw`/`.pcm` ファイルに対してのみ実測値を返す。MP4/MOV/WAV/MP3 等は -23.0 固定値フォールバック |
| DavinciResolveXmlExporter | REAL | `DavinciResolveXmlExporter.cpp:27-95` — QXmlStreamWriter で XMEML v4 形式のシーケンス/クリップ/レート/メディアを完全シリアライズ | 完全スタンドアローン動作 |
| FcpxmlExporter | REAL | `FcpxmlExporter.cpp:55-97` — FCPXML 形式でイベント/シーケンス/クリップを文字列ストリームで生成 | 完全スタンドアローン動作 |
| ProjectTemplate | REAL | `ProjectTemplate.cpp:75-141` — QJsonDocument でテンプレートのシリアライズ・保存・読み込みを実装。ユーザー定義テンプレートのファイル永続化も実装 | 完全スタンドアローン動作 |
| TwitchStreamConfig | REAL | `TwitchStreamConfig.cpp:25-45` — `buildFfmpegCommand()` が RTMP URL とエンコードパラメータ (bitrate/framerate/audioBitrate) から ffmpeg 引数リストを生成。stream key は `config.streamKey` から取得 | コマンド生成のみ。実際のストリーミング開始は呼び出し側が `ffmpeg` プロセスを起動する必要がある。stream key なしでは RTMP が失敗する |
| BatchExportQueue | STUB | `BatchExportQueue.cpp:14-28` — 進捗は `QTimer` が 20% ずつインクリメントするだけで、実際のエクスポート処理は一切行われない。外部エクスポーターへの呼び出しはなし | キューのUIとステート管理はあるが、実エクスポートはno-op |

---

## push-readiness 総括

### 配信系 (YouTube / Vimeo / X / Instagram / CloudRender)

これらはすべて **PARTIAL** 評価である。OAuth フローおよびAPIリクエスト構築のロジックは実装されているが、実際の動作には外部クレデンシャルが必須であり、現状のデフォルト設定では動作しない。

- **YouTube**: `YoutubeOAuthConfig::defaultConfig()` は `clientId/clientSecret` を空文字列で初期化する。GCP の OAuth 2.0 クライアントID/シークレットを注入するまでトークン交換フローが完了しない。アップロード自体は resumable upload として正しく実装されている。
- **Vimeo**: `VEDITOR_VIMEO_CLIENT_ID` / `VEDITOR_VIMEO_CLIENT_SECRET` 環境変数がなければ `dummy-vimeo-client-id` / `dummy-vimeo-client-secret` で動作し、Vimeo API に 401 を返させる (`VimeoOAuth.cpp:30-33`)。
- **X (Twitter)**: `VEDITOR_X_BEARER_TOKEN` が未設定だと `XVideoUpload.cpp:45` で "`upload will be a no-op`" の警告が出て即失敗する (`XVideoUpload.cpp:61`)。
- **Instagram**: `VEDITOR_IG_ACCESS_TOKEN` および ig_user_id なしでは `InstagramPublish.cpp:56,60` で即 `publishFailed` を emit する。
- **CloudRenderClient**: endpointUrl 設定が必要。設定なしでは QUrl が不正になりネットワーク呼び出しが失敗する。AWS Batch / GCP Cloud Run / 汎用 REST の 3 プロバイダに対応したリクエスト構築は実装済み。

**結論**: 配信系は request-level のロジックは検証済みだが、実際のアップロードは一切行われていない。これを「動作する」と説明するのは不正確である。

### 字幕翻訳 (SubtitleTranslator)

- **Stub プロバイダ**: オフラインで動作する。翻訳結果は `"[lang] <元のテキスト>"` という形式の文字列であり、実用的な翻訳ではない。
- **Google Translate V2 / DeepL**: API キーが空なら即失敗する。キーがあれば実動する実装になっている。

### 実 footage デコード経路 (ColorMatchAnalyzer / SmartEditAssistant)

- **ColorMatchAnalyzer**: `analyzeFrameRange()` は `avformat_open_input → avcodec_open2 → av_read_frame → avcodec_receive_frame` の実デコードパスを持つ。libavformat/libavcodec にリンクされているビルドであれば実動する。デコード失敗時は qWarning を出して空の ColorStats を返す（クラッシュしない）。
- **SmartEditAssistant**: `analyze()` は `AutoEdit::detectSilenceFromFile()` と `AutoEdit::detectSceneChanges()` に委譲しており、後者は libavformat/libavcodec で実デコードする (`AutoEdit.cpp:91-183`)。独自にデコードはせずラッパーとして機能する。スタブはない。

### AudioRestoration / LoudnessMaster

- **AudioRestoration**: `deClick / deHum / spectralNoiseGate / processAll` は完全な DSP 実装であるが、API は **インメモリ `QVector<float>` のみ**を受け取る。ファイルを入力として受け取るエントリポイントが存在しない。UI (AudioRestorationDialog) が実際にどのようにサンプルを調達するかは Dialog 側の実装に依存する。DSP ロジック単体は正しい。
- **LoudnessMaster**: `measureIntegratedLufsFromSamples()` は ITU-R BS.1770-4 準拠の実装（K重み付けフィルタ、ゲーティング）である。しかし `measureIntegratedLufs(filePath)` は `.raw`/`.pcm` ファイル（48kHz モノラル raw float を仮定）のみ実測し、**それ以外のすべての拡張子に対して -23.0 LUFS のハードコード値を返す** (`LoudnessMaster.cpp:183-187`)。確認済み。

### 純粋ロジックとして信頼できるモジュール（外部クレデンシャル不要・即実動）

以下のモジュールはスタンドアローンで動作し、信頼できる出力を生成する:

- **DavinciResolveXmlExporter** — XMEML v4 XML の完全実装
- **FcpxmlExporter** — FCPXML の完全実装
- **ColorMatchAnalyzer + ColorMatchLutGenerator** — libavformat デコード + 3D LUT 生成 + .cube エクスポート
- **ChromaKeyRefine** — HSV距離ベースのアルファ合成 + スピルサプレッション
- **HdrGrading** — ST 2084 PQ / HLG EOTF の数学的実装
- **LowerThirdTemplates** — QPainter によるアニメーション付きテロップレンダリング
- **WatermarkOverlay** — QPainter による透明度合成ウォーターマーク
- **ProjectTemplate** — JSON テンプレートの保存/読み込み
- **TwitchStreamConfig** — ffmpeg RTMP コマンド生成（stream key は必要だが、ロジック自体はシンプルで正しい）
- **SubtitleTranslator (Stub モード)** — オフラインで動作（翻訳品質は除く）
- **SmartEditAssistant** — libavformat 経由の実デコード (AutoEdit委譲)
- **MultiCamSync** — WaveformGenerator (libavformat) 経由のクロス相関同期

### 要注意モジュール

- **BatchExportQueue**: STUB。QTimer で進捗をインクリメントするだけで実エクスポートは一切行われない。唯一の真のスタブ評価。
- **AnimatedExport (WebP)**: QtのimageformatsプラグインがアニメーションWebPをサポートしない場合、1フレームのみ書き出す。GIFは常に動作する。
- **LoudnessMaster**: MP4/WAV等のファイルへの呼び出しは -23.0 LUFS を返すため、実測と誤解させるリスクがある。

---

## 評価カウント

| 区分 | 数 | モジュール |
|---|---|---|
| **REAL** | 10 | DavinciResolveXmlExporter, FcpxmlExporter, ChromaKeyRefine, HdrGrading, LowerThirdTemplates, WatermarkOverlay, ProjectTemplate, TwitchStreamConfig, ColorMatchAnalyzer, ColorMatchLutGenerator |
| **PARTIAL** | 14 | YoutubeOAuth, YoutubeUploadClient, YoutubeUploadManager, VimeoOAuth, VimeoUploadClient, VimeoUploadManager, XVideoUpload, InstagramPublish, CloudRenderClient, FrameIoImporter, SubtitleTranslator, AudioRestoration, AnimatedExport, MultiCamSync, LoudnessMaster, SmartEditAssistant |
| **STUB** | 1 | BatchExportQueue |

*(PARTIAL のカウントが14、合計25 モジュール)*
