package com.zheliu.pokecapsule.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.graphics.Typeface;
import android.media.MediaPlayer;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.service.DeviceRuntimeProfile;
import com.zheliu.pokecapsule.service.LibraryChangeNotifier;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.service.TranscriptionScheduler;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.PokePaths;
import com.zheliu.pokecapsule.transcription.TencentAsrConfig;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class CapsuleDetailActivity extends Activity {
    private final CapsuleStore store = new CapsuleStore(new PokePaths());
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private String capsuleId;
    private CapsuleRecord record;
    private TextView title;
    private TextView metadata;
    private TextView stateChip;
    private TextView primaryTextLabel;
    private TextView bestText;
    private TextView errorText;
    private TextView retryButton;
    private TextView playbackButton;
    private TextView favoriteButton;
    private TextView versionsToggle;
    private LinearLayout errorCard;
    private LinearLayout versions;
    private TextView raw;
    private TextView polished;
    private TextView finalText;
    private MediaPlayer player;
    private boolean versionsExpanded;
    private boolean libraryReceiverRegistered;

    private final BroadcastReceiver libraryChangeReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            load();
            if (TencentAsrConfig.isConfigured(CapsuleDetailActivity.this)) {
                TranscriptionScheduler.scheduleAutomatic(CapsuleDetailActivity.this);
            }
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        capsuleId = getIntent().getStringExtra("capsuleId");
        setContentView(buildPage());
    }

    @Override public void onResume() {
        super.onResume();
        load();
        if (TencentAsrConfig.isConfigured(this)) {
            TranscriptionScheduler.scheduleAutomatic(this);
        }
    }

    @Override public void onStart() {
        super.onStart();
        registerReceiver(
                libraryChangeReceiver,
                new IntentFilter(LibraryChangeNotifier.ACTION),
                LibraryChangeNotifier.INTERNAL_PERMISSION,
                null);
        libraryReceiverRegistered = true;
    }

    @Override public void onStop() {
        if (libraryReceiverRegistered) {
            unregisterReceiver(libraryChangeReceiver);
            libraryReceiverRegistered = false;
        }
        super.onStop();
    }

    @Override public void onDestroy() {
        stopPlayback();
        io.shutdownNow();
        super.onDestroy();
    }

    private ScrollView buildPage() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(ViewKit.background(this));
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(18), dp(18), dp(18), dp(28));
        page.setBackgroundColor(ViewKit.background(this));
        scroll.addView(page);

        LinearLayout header = row();
        title = ViewKit.text(this, "胶囊", 26, Typeface.BOLD);
        title.setMaxLines(2);
        header.addView(title, new LinearLayout.LayoutParams(0, -2, 1f));
        favoriteButton = ViewKit.quietButton(this, "收藏", v -> toggleFavorite());
        header.addView(favoriteButton, lp(dp(88), dp(48)));
        page.addView(header, block());

        metadata = ViewKit.text(this, "读取中…", 15, Typeface.NORMAL);
        metadata.setTextColor(ViewKit.secondary(this));
        metadata.setPadding(0, 0, 0, dp(8));
        page.addView(metadata, lp(-1, -2));

        stateChip = ViewKit.status(this, "读取中", false);
        LinearLayout.LayoutParams stateParams = lp(-2, dp(34));
        stateParams.bottomMargin = dp(18);
        page.addView(stateChip, stateParams);

        errorCard = ViewKit.card(this);
        TextView errorHeading = ViewKit.sectionTitle(this, "这段录音需要处理");
        errorHeading.setTextColor(ViewKit.error(this));
        errorCard.addView(errorHeading);
        errorText = ViewKit.text(this, "", 16, Typeface.NORMAL);
        errorText.setTextColor(ViewKit.error(this));
        errorText.setPadding(0, 0, 0, dp(10));
        errorCard.addView(errorText);
        retryButton = ViewKit.secondaryButton(this, "修复并重新转写", v -> retry());
        errorCard.addView(retryButton, lp(-1, dp(48)));
        errorCard.setVisibility(View.GONE);
        page.addView(errorCard, block());

        LinearLayout audioCard = ViewKit.card(this);
        audioCard.addView(ViewKit.sectionTitle(this, "录音"));
        playbackButton = ViewKit.primaryButton(this, "播放录音", v -> togglePlayback());
        audioCard.addView(playbackButton, lp(-1, dp(50)));
        page.addView(audioCard, block());

        LinearLayout textCard = ViewKit.card(this);
        primaryTextLabel = ViewKit.sectionTitle(this, "胶囊文字");
        textCard.addView(primaryTextLabel);
        bestText = ViewKit.text(this, "转写完成后，文字会出现在这里。", 18, Typeface.NORMAL);
        bestText.setTextIsSelectable(true);
        bestText.setGravity(Gravity.TOP);
        bestText.setLineSpacing(0, 1.15f);
        bestText.setPadding(0, 0, 0, dp(14));
        textCard.addView(bestText, lp(-1, -2));
        LinearLayout textActions = row();
        textActions.addView(
                ViewKit.secondaryButton(this, "编辑", v -> promptFinalText()), weight());
        textActions.addView(
                ViewKit.quietButton(this, "复制", v -> copyFinalText()), weight());
        textCard.addView(textActions, lp(-1, dp(50)));
        page.addView(textCard, block());

        page.addView(ViewKit.sectionTitle(this, "整理"));
        LinearLayout organize = row();
        organize.addView(ViewKit.quietButton(this, "改标题", v -> promptTitle()), weight());
        organize.addView(ViewKit.quietButton(this, "加标签", v -> promptTag(true)), weight());
        organize.addView(ViewKit.quietButton(this, "移标签", v -> promptTag(false)), weight());
        page.addView(organize, block());

        versionsToggle = ViewKit.quietButton(this, "查看文字版本", v -> toggleVersions());
        page.addView(versionsToggle, lp(-1, dp(48)));
        versions = ViewKit.card(this);
        versions.setVisibility(View.GONE);
        versions.addView(ViewKit.sectionTitle(this, "原始转写"));
        raw = ViewKit.text(this, "", 16, Typeface.NORMAL);
        raw.setTextIsSelectable(true);
        raw.setPadding(0, 0, 0, dp(16));
        versions.addView(raw);
        versions.addView(ViewKit.sectionTitle(this, "校对文字"));
        polished = ViewKit.text(this, "", 16, Typeface.NORMAL);
        polished.setTextIsSelectable(true);
        polished.setPadding(0, 0, 0, dp(16));
        versions.addView(polished);
        versions.addView(ViewKit.sectionTitle(this, "最终文字"));
        finalText = ViewKit.text(this, "", 16, Typeface.NORMAL);
        finalText.setTextIsSelectable(true);
        versions.addView(finalText);
        LinearLayout.LayoutParams versionsParams = block();
        versionsParams.topMargin = dp(10);
        page.addView(versions, versionsParams);
        return scroll;
    }

    private void load() {
        io.execute(() -> {
            try {
                File directory = store.paths().findCapsuleById(capsuleId);
                if (directory == null) throw new Exception("胶囊已移动或不存在");
                CapsuleRecord loaded = store.readCapsule(directory);
                String rawText = readOptional(new File(directory, "raw.txt"));
                String polishedText = readOptional(new File(directory, "polished.md"));
                String finalValue = readOptional(new File(directory, "final.md"));
                runOnUiThread(() -> {
                    record = loaded;
                    title.setText(loaded.title);
                    metadata.setText(loaded.metadataText()
                            + (loaded.tagsText().isEmpty() ? "" : "\n" + loaded.tagsText()));
                    String status = statusLabel(loaded);
                    stateChip.setText(status);
                    stateChip.setVisibility(status.isEmpty() ? View.GONE : View.VISIBLE);
                    favoriteButton.setText(loaded.favorite ? "★ 已收藏" : "☆ 收藏");
                    applyError(loaded);
                    String primary = primaryText(loaded, finalValue, polishedText, rawText);
                    bestText.setText(primary.isEmpty()
                            ? "转写完成后，文字会出现在这里。"
                            : primary);
                    primaryTextLabel.setText(finalValue.isEmpty()
                            ? (polishedText.isEmpty() && rawText.isEmpty()
                                    ? "胶囊文字"
                                    : "转写文字")
                            : "最终文字");
                    raw.setText(rawText.isEmpty() ? "暂无" : rawText);
                    polished.setText(polishedText.isEmpty() ? "暂无" : polishedText);
                    finalText.setText(finalValue.isEmpty() ? "暂无" : finalValue);
                    boolean hasVersions = !rawText.isEmpty()
                            || !polishedText.isEmpty()
                            || !finalValue.isEmpty();
                    versionsToggle.setVisibility(hasVersions ? View.VISIBLE : View.GONE);
                    if (!hasVersions) {
                        versionsExpanded = false;
                        versions.setVisibility(View.GONE);
                    }
                });
            } catch (Exception error) {
                runOnUiThread(() -> toast(error.getMessage()));
            }
        });
    }

    private void promptFinalText() {
        if (record == null || record.readOnly) return;
        String initial = record.finalText;
        if (initial == null || initial.isEmpty()) {
            initial = !record.polishedText.isEmpty() ? record.polishedText : record.rawText;
        }
        EditText input = new EditText(this);
        input.setText(initial);
        input.setMinLines(6);
        input.setGravity(android.view.Gravity.TOP);
        new AlertDialog.Builder(this)
                .setTitle("编辑最终文字")
                .setView(input)
                .setPositiveButton("保存", (dialog, which) ->
                        runOperation(
                                () -> store.setFinalText(record.id, input.getText().toString()),
                                "最终文字已保存"))
                .setNegativeButton("取消", null)
                .show();
    }

    private void copyFinalText() {
        if (record == null) return;
        String value = record.previewText();
        android.content.ClipboardManager clipboard =
                (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(android.content.ClipData.newPlainText("PokeCapsule", value));
        toast("文字已复制");
    }

    private void togglePlayback() {
        if (record == null) return;
        if (player != null) {
            stopPlayback();
            return;
        }
        File audio = new File(record.directory, "audio.m4a");
        try {
            player = new MediaPlayer();
            player.setDataSource(audio.getAbsolutePath());
            player.setOnCompletionListener(value -> stopPlayback());
            player.prepare();
            player.start();
            playbackButton.setText("停止播放");
            toast(DeviceRuntimeProfile.isLowPowerReader()
                    ? "正在播放；Poke3 需连接蓝牙或 USB 音频设备"
                    : "正在通过手机扬声器播放");
        } catch (Exception error) {
            stopPlayback();
            toast("无法播放: " + error.getMessage());
        }
    }

    private void promptTitle() {
        if (record == null || record.readOnly) return;
        prompt("修改标题", record.title, value ->
                runOperation(() -> store.setTitle(record.id, value), "标题已更新"));
    }

    private void toggleFavorite() {
        if (record == null || record.readOnly) return;
        runOperation(
                () -> store.setFavorite(Collections.singletonList(record.id), !record.favorite),
                record.favorite ? "已取消收藏" : "已收藏");
    }

    private void promptTag(boolean add) {
        if (record == null || record.readOnly) return;
        prompt(add ? "添加标签" : "移除标签", "", value ->
                runOperation(() -> {
                    if (add) store.addTag(Collections.singletonList(record.id), value);
                    else store.removeTag(Collections.singletonList(record.id), value);
                }, "标签已更新"));
    }

    private void retry() {
        if (record == null || record.readOnly) return;
        runOperation(() -> {
            store.requeueFailedTranscriptions(Collections.singletonList(record.id));
            TranscriptionScheduler.scheduleManual(this);
        }, "已重新排队");
    }

    private void runOperation(Operation operation, String message) {
        io.execute(() -> {
            try {
                operation.run();
                runOnUiThread(() -> {
                    toast(message);
                    load();
                });
            } catch (Exception error) {
                runOnUiThread(() -> toast("操作失败: " + error.getMessage()));
            }
        });
    }

    private void prompt(String heading, String initial, TextResult callback) {
        EditText input = new EditText(this);
        input.setText(initial);
        input.setSelection(input.length());
        new AlertDialog.Builder(this)
                .setTitle(heading)
                .setView(input)
                .setPositiveButton("确定", (dialog, which) -> callback.accept(input.getText().toString()))
                .setNegativeButton("取消", null)
                .show();
    }

    private void stopPlayback() {
        if (player != null) {
            try {
                player.stop();
            } catch (IllegalStateException ignored) {
            }
            player.release();
            player = null;
        }
        if (playbackButton != null) playbackButton.setText("播放录音");
    }

    private void toggleVersions() {
        versionsExpanded = !versionsExpanded;
        versions.setVisibility(versionsExpanded ? View.VISIBLE : View.GONE);
        versionsToggle.setText(versionsExpanded ? "收起文字版本" : "查看文字版本");
    }

    private void applyError(CapsuleRecord loaded) {
        String message = friendlyError(loaded.error);
        boolean visible = !message.isEmpty() || loaded.status == ProcessingState.FAILED;
        errorCard.setVisibility(visible ? View.VISIBLE : View.GONE);
        if (!visible) return;
        errorText.setText(message.isEmpty()
                ? "转写没有完成，原始录音仍然安全保存。"
                : message);
        boolean configure = isConfigurationFailure(loaded.error);
        boolean canRetry = loaded.status == ProcessingState.FAILED
                && canRetryTranscription(loaded.error);
        retryButton.setVisibility(canRetry || configure ? View.VISIBLE : View.GONE);
        if (configure) {
            retryButton.setText("打开转写设置");
            retryButton.setOnClickListener(v -> openTranscriptionSettings());
        } else {
            retryButton.setText(containsDurationLimit(loaded.error)
                    ? "生成安全副本并重新转写"
                    : "重新转写");
            retryButton.setOnClickListener(v -> retry());
        }
    }

    private static String friendlyError(String value) {
        if (value == null || value.trim().isEmpty()) return "";
        if (value.contains("ErrorVoicedataTooLong") || value.contains("longer than 60 seconds")) {
            return "录音略微超过云端的 60 秒上限。应用会保留原音，使用安全副本重新转写。";
        }
        if (value.startsWith("InvalidParameter")
                || value.startsWith("UnsupportedOperation")) {
            return "这段录音暂时无法转写，原始录音仍然安全保存。";
        }
        if (value.startsWith("AuthFailure")) {
            return "转写服务配置失效，请在设置中重新导入。";
        }
        if (value.contains("Exception") || value.contains("Error")) {
            return "转写暂时没有完成，原始录音仍然安全保存。";
        }
        return value;
    }

    private static boolean canRetryTranscription(String value) {
        if (value == null || value.trim().isEmpty()) return true;
        if (value.startsWith("AuthFailure")
                || value.contains("配置失效")
                || value.contains("自动裁剪失败")
                || value.contains("超过自动修复范围")
                || value.contains("音轨")
                || value.contains("本地音频")) {
            return false;
        }
        return containsDurationLimit(value)
                || value.contains("网络")
                || value.contains("服务")
                || value.contains("稍后")
                || value.contains("连续失败");
    }

    private static boolean isConfigurationFailure(String value) {
        return value != null && (value.startsWith("AuthFailure")
                || value.contains("配置失效"));
    }

    private static boolean containsDurationLimit(String value) {
        return value != null && (value.contains("ErrorVoicedataTooLong")
                || value.contains("longer than 60 seconds"));
    }

    private void openTranscriptionSettings() {
        Intent intent = new Intent(this, MainActivity.class);
        intent.putExtra(MainActivity.EXTRA_OPEN_SETTINGS, true);
        intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(intent);
        finish();
    }

    private static String primaryText(
            CapsuleRecord record,
            String finalValue,
            String polishedValue,
            String rawValue) {
        if (finalValue.trim().isEmpty()
                && polishedValue.trim().isEmpty()
                && rawValue.trim().isEmpty()) {
            return "";
        }
        return record.previewText();
    }

    private static String statusLabel(CapsuleRecord value) {
        switch (value.status) {
            case RECORDING: return "录音中";
            case RECORDED:
            case QUEUED: return "等待转写";
            case TRANSCRIBING: return "正在转写";
            case RAW_READY: return "转写完成";
            case CORRECTING: return "正在校对";
            case READY: return "已整理";
            case FAILED: return "需要处理";
            default: return "";
        }
    }

    private static String readOptional(File file) throws Exception {
        if (!file.isFile()) return "";
        try (FileInputStream input = new FileInputStream(file);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }

    private static String tagsLine(CapsuleRecord record) {
        if (record.tags.isEmpty()) return "无标签";
        StringBuilder result = new StringBuilder();
        for (String tag : record.tags) {
            if (result.length() > 0) result.append(' ');
            result.append('#').append(tag);
        }
        return result.toString();
    }

    private LinearLayout row() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    private LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams value = new LinearLayout.LayoutParams(0, -1, 1f);
        value.setMargins(dp(2), dp(3), dp(2), dp(3));
        return value;
    }

    private LinearLayout.LayoutParams lp(int width, int height) {
        return new LinearLayout.LayoutParams(width, height);
    }

    private LinearLayout.LayoutParams block() {
        LinearLayout.LayoutParams value = lp(-1, -2);
        value.bottomMargin = dp(18);
        return value;
    }

    private int dp(int value) {
        return ViewKit.dp(this, value);
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private interface Operation {
        void run() throws Exception;
    }

    private interface TextResult {
        void accept(String value);
    }
}
