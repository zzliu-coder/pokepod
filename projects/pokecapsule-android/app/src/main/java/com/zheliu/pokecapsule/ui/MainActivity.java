package com.zheliu.pokecapsule.ui;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.zheliu.pokecapsule.core.PathPolicy;
import com.zheliu.pokecapsule.core.ProcessingState;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.service.DeviceRuntimeProfile;
import com.zheliu.pokecapsule.service.LibraryChangeNotifier;
import com.zheliu.pokecapsule.service.OverlayService;
import com.zheliu.pokecapsule.service.RecordingService;
import com.zheliu.pokecapsule.service.TranscriptionScheduler;
import com.zheliu.pokecapsule.service.TranscriptionPolicyText;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.DeviceIdentity;
import com.zheliu.pokecapsule.storage.PokePaths;
import com.zheliu.pokecapsule.storage.SearchIndex;
import com.zheliu.pokecapsule.transcription.TencentAsrConfig;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

public final class MainActivity extends Activity {
    private static final int REQUEST_PERMISSIONS = 91;
    private static final int REQUEST_TENCENT_CONFIG = 92;
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final CapsuleStore store = new CapsuleStore(new PokePaths());
    private final ArrayList<CapsuleRecord> visible = new ArrayList<>();
    private final Set<String> selected = new HashSet<>();
    private ArrayAdapter<CapsuleRecord> adapter;
    private TextView heading;
    private TextView selectionBar;
    private TextView moveAction;
    private TextView copyAction;
    private TextView tagAction;
    private TextView favoriteAction;
    private TextView deleteAction;
    private String folderFilter = PathPolicy.INBOX;
    private String tagFilter;
    private boolean favoritesOnly;
    private boolean trashOnly;
    private String statusFilter;
    private String searchQuery = "";
    private boolean cloudConfigured;
    private boolean libraryReceiverRegistered;
    private boolean inlineRecording;
    private CapsuleRecordButtonView inlineRecordButton;

    private final BroadcastReceiver libraryChangeReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if (RecordingService.ACTION_STATE.equals(intent.getAction())) {
                inlineRecording = intent.getBooleanExtra(
                        RecordingService.EXTRA_RECORDING, false);
                if (inlineRecordButton != null) {
                    if (inlineRecording) {
                        inlineRecordButton.showRecording(
                                intent.getIntExtra(RecordingService.EXTRA_SECONDS_LEFT, 0),
                                intent.getIntExtra(RecordingService.EXTRA_AUDIO_LEVEL, 0),
                                intent.getBooleanExtra(RecordingService.EXTRA_SILENT, false));
                    } else {
                        inlineRecordButton.showIdle(
                                intent.getStringExtra(RecordingService.EXTRA_MESSAGE));
                    }
                }
                return;
            }
            cloudConfigured = TencentAsrConfig.isConfigured(MainActivity.this);
            refresh();
            if (cloudConfigured) TranscriptionScheduler.scheduleAutomatic(MainActivity.this);
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(buildPage());
        requestRequiredPermissions();
    }

    @Override public void onResume() {
        super.onResume();
        cloudConfigured = TencentAsrConfig.isConfigured(this);
        refresh();
        if (cloudConfigured) TranscriptionScheduler.scheduleAutomatic(this);
    }

    @Override public void onStart() {
        super.onStart();
        IntentFilter filter = new IntentFilter(LibraryChangeNotifier.ACTION);
        filter.addAction(RecordingService.ACTION_STATE);
        registerReceiver(
                libraryChangeReceiver,
                filter,
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
        io.shutdownNow();
        super.onDestroy();
    }

    private View buildPage() {
        boolean lowPowerReader = DeviceRuntimeProfile.isLowPowerReader();
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(14), dp(10), dp(14), dp(10));
        page.setBackgroundColor(Color.WHITE);

        heading = ViewKit.text(this, "PokeCapsule · Inbox", 25, Typeface.BOLD);
        page.addView(heading, lp(-1, dp(lowPowerReader ? 46 : 72)));

        LinearLayout tabs = row();
        tabs.addView(smallButton("Inbox", v -> showInbox()), weight());
        tabs.addView(smallButton("目录", v -> showFolderChooser()), weight());
        tabs.addView(smallButton("标签", v -> showTagChooser()), weight());
        tabs.addView(smallButton("收藏", v -> showFavorites()), weight());
        tabs.addView(smallButton("设置", v -> showSettings()), weight());
        page.addView(tabs, lp(-1, dp(48)));

        LinearLayout tools = row();
        tools.addView(smallButton("搜索", v -> promptSearch()), weight());
        tools.addView(smallButton("待转写", v -> showSmart("pending")), weight());
        tools.addView(smallButton("失败", v -> showSmart("failed")), weight());
        tools.addView(smallButton("回收站", v -> showTrash()), weight());
        page.addView(tools, lp(-1, dp(44)));

        ListView list = new ListView(this);
        list.setDividerHeight(dp(1));
        adapter = new ArrayAdapter<CapsuleRecord>(
                this, android.R.layout.simple_list_item_1, new ArrayList<>()) {
            @Override public View getView(int position, View convertView, ViewGroup parent) {
                CapsuleRecord record = getItem(position);
                LinearLayout row = new LinearLayout(MainActivity.this);
                row.setOrientation(LinearLayout.VERTICAL);
                row.setPadding(dp(10), dp(7), dp(10), dp(7));

                TextView preview = ViewKit.text(
                        MainActivity.this,
                        (selected.contains(record.id) ? "☑ " : "")
                                + (record.favorite ? "★ " : "")
                                + previewText(record),
                        17,
                        Typeface.BOLD);
                preview.setSingleLine(true);
                preview.setEllipsize(TextUtils.TruncateAt.END);
                row.addView(preview, lp(-1, -2));

                TextView metadata = ViewKit.text(
                        MainActivity.this,
                        record.metadataText()
                                + (record.tagsText().isEmpty() ? "" : " · " + record.tagsText()),
                        14,
                        Typeface.NORMAL);
                metadata.setTextColor(Color.DKGRAY);
                metadata.setSingleLine(true);
                metadata.setEllipsize(TextUtils.TruncateAt.END);
                row.addView(metadata, lp(-1, dp(25)));
                return row;
            }
        };
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> {
            CapsuleRecord record = visible.get(position);
            if (selected.isEmpty() && !trashOnly) {
                Intent detail = new Intent(this, CapsuleDetailActivity.class);
                detail.putExtra("capsuleId", record.id);
                startActivity(detail);
            } else {
                toggleSelected(record.id);
            }
        });
        list.setOnItemLongClickListener((parent, view, position, id) -> {
            toggleSelected(visible.get(position).id);
            return true;
        });
        page.addView(list, new LinearLayout.LayoutParams(-1, 0, 1f));

        selectionBar = ViewKit.text(this, "长按胶囊开始多选", 14, Typeface.NORMAL);
        selectionBar.setGravity(Gravity.CENTER);
        page.addView(selectionBar, lp(-1, dp(34)));

        LinearLayout actions = row();
        moveAction = smallButton("移动", v -> primaryMoveAction());
        copyAction = smallButton("复制", v -> promptCopyActions());
        tagAction = smallButton("标签", v -> promptTag());
        favoriteAction = smallButton("收藏", v -> promptFavorite());
        deleteAction = smallButton("删除", v -> confirmDelete());
        actions.addView(moveAction, weight());
        actions.addView(copyAction, weight());
        actions.addView(tagAction, weight());
        actions.addView(favoriteAction, weight());
        actions.addView(deleteAction, weight());
        page.addView(actions, lp(-1, dp(48)));
        if (lowPowerReader) return page;

        FrameLayout screen = new FrameLayout(this);
        screen.setBackgroundColor(Color.WHITE);
        screen.addView(page, new FrameLayout.LayoutParams(-1, -1));

        inlineRecordButton = new CapsuleRecordButtonView(this);
        inlineRecordButton.setOnClickListener(v -> toggleInlineRecording());
        FrameLayout.LayoutParams recordParams =
                new FrameLayout.LayoutParams(dp(64), dp(64), Gravity.RIGHT | Gravity.BOTTOM);
        recordParams.rightMargin = dp(20);
        recordParams.bottomMargin = dp(96);
        screen.addView(inlineRecordButton, recordParams);
        attachInlineRecordDrag(screen);
        return screen;
    }

    private void attachInlineRecordDrag(FrameLayout screen) {
        int touchSlop = ViewConfiguration.get(this).getScaledTouchSlop();
        inlineRecordButton.setOnTouchListener(new View.OnTouchListener() {
            private float downRawX;
            private float downRawY;
            private float startX;
            private float startY;
            private boolean moved;

            @Override public boolean onTouch(View view, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downRawX = event.getRawX();
                        downRawY = event.getRawY();
                        startX = view.getX();
                        startY = view.getY();
                        moved = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = event.getRawX() - downRawX;
                        float dy = event.getRawY() - downRawY;
                        if (Math.abs(dx) > touchSlop || Math.abs(dy) > touchSlop) moved = true;
                        if (moved) {
                            float maxX = Math.max(0, screen.getWidth() - view.getWidth());
                            float maxY = Math.max(0, screen.getHeight() - view.getHeight());
                            view.setX(Math.max(0, Math.min(maxX, startX + dx)));
                            view.setY(Math.max(0, Math.min(maxY, startY + dy)));
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (!moved) view.performClick();
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        return true;
                    default:
                        return false;
                }
            }
        });
    }

    private String previewText(CapsuleRecord record) {
        if ((record.status == ProcessingState.RECORDED
                || record.status == ProcessingState.QUEUED)
                && !cloudConfigured) {
            return "等待配置腾讯转写";
        }
        return record.previewText();
    }

    private void requestRequiredPermissions() {
        ArrayList<String> missing = new ArrayList<>();
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            missing.add(Manifest.permission.RECORD_AUDIO);
        }
        if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            missing.add(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        }
        if (!missing.isEmpty()) {
            requestPermissions(missing.toArray(new String[0]), REQUEST_PERMISSIONS);
        }
    }

    @Override public void onRequestPermissionsResult(
            int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_PERMISSIONS) {
            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) {
                    toast("录音和文件权限未完整授予，可在设置中恢复");
                    break;
                }
            }
            refresh();
        }
    }

    private void refresh() {
        io.execute(() -> {
            try {
                DeviceIdentity.ensure(this, store.paths());
                List<CapsuleRecord> all = trashOnly ? store.scanTrash() : store.scan();
                ArrayList<CapsuleRecord> filtered = new ArrayList<>();
                for (CapsuleRecord record : all) {
                    if (matches(record)) filtered.add(record);
                }
                List<CapsuleRecord> searched = SearchIndex.filter(filtered, searchQuery);
                runOnUiThread(() -> applyRecords(searched));
            } catch (Exception error) {
                runOnUiThread(() -> toast("读取失败: " + error.getMessage()));
            }
        });
    }

    private boolean matches(CapsuleRecord record) {
        if (trashOnly) return true;
        if ("pending".equals(statusFilter)) {
            switch (record.status) {
                case RECORDED:
                case QUEUED:
                case TRANSCRIBING:
                    return true;
                default:
                    return false;
            }
        }
        if ("failed".equals(statusFilter)) {
            return record.status
                    == com.zheliu.pokecapsule.core.ProcessingState.FAILED;
        }
        if (favoritesOnly) return record.favorite;
        if (tagFilter != null) {
            for (String tag : record.tags) if (tagFilter.equalsIgnoreCase(tag)) return true;
            return false;
        }
        if (folderFilter == null) return true;
        File parent = record.directory.getParentFile();
        if (parent == null) return false;
        String relative = store.paths().root().toURI().relativize(parent.toURI()).getPath();
        if (relative.endsWith("/")) relative = relative.substring(0, relative.length() - 1);
        return folderFilter.equals(relative);
    }

    private void applyRecords(List<CapsuleRecord> records) {
        visible.clear();
        visible.addAll(records);
        selected.retainAll(ids(records));
        renderList();
    }

    private void renderList() {
        adapter.clear();
        adapter.addAll(visible);
        adapter.notifyDataSetChanged();
        selectionBar.setText(selected.isEmpty()
                ? "长按胶囊开始多选"
                : "已选择 " + selected.size() + " 个胶囊 · 轻点继续选择");
        moveAction.setText(trashOnly ? "恢复" : "移动");
        copyAction.setText(trashOnly ? "复制文字" : "复制");
        tagAction.setEnabled(!trashOnly);
        favoriteAction.setEnabled(!trashOnly);
        deleteAction.setText(trashOnly ? "永久删除" : "删除");
    }

    private void showInbox() {
        folderFilter = PathPolicy.INBOX;
        tagFilter = null;
        favoritesOnly = false;
        trashOnly = false;
        statusFilter = null;
        searchQuery = "";
        selected.clear();
        heading.setText("PokeCapsule · Inbox");
        refresh();
    }

    private void showFavorites() {
        folderFilter = null;
        tagFilter = null;
        favoritesOnly = true;
        trashOnly = false;
        statusFilter = null;
        searchQuery = "";
        selected.clear();
        heading.setText("PokeCapsule · 收藏");
        refresh();
    }

    private void showFolderChooser() {
        io.execute(() -> {
            try {
                ArrayList<String> choices = new ArrayList<>();
                choices.add(PathPolicy.INBOX);
                choices.add(PathPolicy.ARCHIVE);
                choices.addAll(store.allUserFolders());
                runOnUiThread(() -> new AlertDialog.Builder(this)
                        .setTitle("目录")
                        .setItems(choices.toArray(new String[0]), (dialog, which) -> {
                            folderFilter = choices.get(which);
                            tagFilter = null;
                            favoritesOnly = false;
                            trashOnly = false;
                            statusFilter = null;
                            searchQuery = "";
                            selected.clear();
                            heading.setText("PokeCapsule · " + folderFilter);
                            refresh();
                        })
                        .setPositiveButton("新建", (dialog, which) -> promptCreateFolder())
                        .setNeutralButton("删除目录", (dialog, which) -> promptDeleteFolder())
                        .setNegativeButton("取消", null)
                        .show());
            } catch (IOException error) {
                runOnUiThread(() -> toast(error.getMessage()));
            }
        });
    }

    private void promptCreateFolder() {
        promptText("新建目录", "一级目录 或 一级/二级", value ->
                runStoreOperation(() -> store.createFolder(value), "目录已创建"));
    }

    private void promptDeleteFolder() {
        promptText("删除目录", "输入完整目录路径", value ->
                new AlertDialog.Builder(this)
                        .setTitle("确认删除目录")
                        .setMessage("目录内全部胶囊会移回 Inbox。")
                        .setPositiveButton("确认", (d, w) ->
                                runStoreOperation(
                                        () -> store.deleteFolderMovingContentsToInbox(value),
                                        "目录已删除，内容已回 Inbox"))
                        .setNegativeButton("取消", null)
                        .show());
    }

    private void showTagChooser() {
        io.execute(() -> {
            try {
                ArrayList<String> tags = new ArrayList<>(store.allTags());
                Collections.sort(tags);
                runOnUiThread(() -> new AlertDialog.Builder(this)
                        .setTitle("标签")
                        .setItems(tags.toArray(new String[0]), (dialog, which) -> {
                            tagFilter = tags.get(which);
                            folderFilter = null;
                            favoritesOnly = false;
                            trashOnly = false;
                            statusFilter = null;
                            searchQuery = "";
                            selected.clear();
                            heading.setText("PokeCapsule · #" + tagFilter);
                            refresh();
                        })
                        .setPositiveButton("改名/合并", (dialog, which) -> promptRenameTag())
                        .setNegativeButton("取消", null)
                        .show());
            } catch (IOException error) {
                runOnUiThread(() -> toast(error.getMessage()));
            }
        });
    }

    private void promptRenameTag() {
        promptText("原标签", "不含 #", oldTag ->
                promptText("新标签", "同名即合并", newTag ->
                        runStoreOperation(() -> store.renameTag(oldTag, newTag), "标签已更新")));
    }

    private void chooseDestination(boolean copy) {
        List<String> ids = selectedIdsOrWarn();
        if (ids == null) return;
        io.execute(() -> {
            try {
                ArrayList<String> destinations = new ArrayList<>();
                destinations.add(PathPolicy.INBOX);
                destinations.add(PathPolicy.ARCHIVE);
                destinations.addAll(store.allUserFolders());
                runOnUiThread(() -> new AlertDialog.Builder(this)
                        .setTitle(copy ? "复制到" : "移动到")
                        .setItems(destinations.toArray(new String[0]), (dialog, which) ->
                                runStoreOperation(() -> {
                                    if (copy) store.copyCapsules(ids, destinations.get(which));
                                    else store.moveCapsules(ids, destinations.get(which));
                                }, copy ? "复制完成" : "移动完成"))
                        .setNegativeButton("取消", null)
                        .show());
            } catch (IOException error) {
                runOnUiThread(() -> toast(error.getMessage()));
            }
        });
    }

    private void primaryMoveAction() {
        if (!trashOnly) {
            chooseDestination(false);
            return;
        }
        List<String> ids = selectedIdsOrWarn();
        if (ids == null) return;
        runStoreOperation(() -> store.restoreCapsules(ids), "已恢复胶囊");
    }

    private void promptCopyActions() {
        List<String> ids = selectedIdsOrWarn();
        if (ids == null) return;
        if (trashOnly) {
            copySelectedText(ids, false);
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("复制")
                .setItems(new String[]{"复制文字", "复制 Markdown", "复制胶囊到目录"},
                        (dialog, which) -> {
                            if (which == 0) copySelectedText(ids, false);
                            else if (which == 1) copySelectedText(ids, true);
                            else chooseDestination(true);
                        })
                .setNegativeButton("取消", null)
                .show();
    }

    private void copySelectedText(List<String> ids, boolean markdown) {
        StringBuilder output = new StringBuilder();
        for (CapsuleRecord record : visible) {
            if (!ids.contains(record.id)) continue;
            if (output.length() > 0) output.append(markdown ? "\n\n---\n\n" : "\n\n");
            if (markdown) {
                output.append("## ").append(record.title).append("\n\n")
                        .append(record.previewText()).append("\n\n")
                        .append(record.metadataText());
                if (!record.tagsText().isEmpty()) {
                    output.append("\n\n").append(record.tagsText());
                }
            } else {
                output.append(record.previewText());
            }
        }
        ClipboardManager clipboard =
                (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(ClipData.newPlainText("PokeCapsule", output.toString()));
        toast("已复制 " + ids.size() + " 条胶囊文字");
    }

    private void promptTag() {
        List<String> ids = selectedIdsOrWarn();
        if (ids == null) return;
        promptText("批量标签", "输入标签，不含 #", tag ->
                new AlertDialog.Builder(this)
                        .setTitle("#" + PathPolicy.normalizeTag(tag))
                        .setItems(new String[]{"添加", "移除"}, (dialog, which) ->
                                runStoreOperation(() -> {
                                    if (which == 0) store.addTag(ids, tag);
                                    else store.removeTag(ids, tag);
                                }, "标签已更新"))
                        .show());
    }

    private void promptFavorite() {
        List<String> ids = selectedIdsOrWarn();
        if (ids == null) return;
        new AlertDialog.Builder(this)
                .setTitle("收藏")
                .setItems(new String[]{"设为收藏", "取消收藏"}, (dialog, which) ->
                        runStoreOperation(() -> store.setFavorite(ids, which == 0), "收藏状态已更新"))
                .show();
    }

    private void confirmDelete() {
        List<String> ids = selectedIdsOrWarn();
        if (ids == null) return;
        new AlertDialog.Builder(this)
                .setTitle((trashOnly ? "永久删除 " : "删除 ") + ids.size() + " 个胶囊？")
                .setMessage(trashOnly
                        ? "录音和文字会永久删除，无法恢复。"
                        : "胶囊会进入回收站，可随时恢复。")
                .setPositiveButton(trashOnly ? "永久删除" : "删除", (dialog, which) ->
                        runStoreOperation(
                                () -> {
                                    if (trashOnly) store.purgeCapsules(ids);
                                    else store.deleteCapsules(ids);
                                },
                                trashOnly ? "已永久删除" : "已移入回收站"))
                .setNegativeButton("取消", null)
                .show();
    }

    private void promptSearch() {
        promptText("全文搜索", "文字、标签或目录", value -> {
            searchQuery = value == null ? "" : value.trim();
            heading.setText(searchQuery.isEmpty()
                    ? "PokeCapsule · 全部"
                    : "搜索 · " + searchQuery);
            folderFilter = null;
            tagFilter = null;
            favoritesOnly = false;
            trashOnly = false;
            statusFilter = null;
            selected.clear();
            refresh();
        });
    }

    private void showSmart(String filter) {
        folderFilter = null;
        tagFilter = null;
        favoritesOnly = false;
        trashOnly = false;
        statusFilter = filter;
        searchQuery = "";
        selected.clear();
        heading.setText("pending".equals(filter)
                ? "PokeCapsule · 待转写"
                : "PokeCapsule · 转写失败");
        refresh();
    }

    private void showTrash() {
        folderFilter = null;
        tagFilter = null;
        favoritesOnly = false;
        trashOnly = true;
        statusFilter = null;
        searchQuery = "";
        selected.clear();
        heading.setText("PokeCapsule · 回收站");
        refresh();
    }

    private void showSettings() {
        cloudConfigured = TencentAsrConfig.isConfigured(this);
        boolean adbEnabled = Settings.Global.getInt(
                getContentResolver(), Settings.Global.ADB_ENABLED, 0) == 1;
        String usbConfig = readProperty("sys.usb.config");
        String computerStatus = adbEnabled && usbConfig.contains("adb")
                ? "电脑管理：ADB 已就绪"
                : adbEnabled ? "电脑管理：调试已开，等待 USB"
                : "电脑管理：USB 调试未开启";
        if (!DeviceRuntimeProfile.isLowPowerReader()) {
            showPhoneSettings(computerStatus);
            return;
        }
        String[] options = {
                Settings.canDrawOverlays(this) ? "开启悬浮按钮" : "授予悬浮窗权限",
                "临时隐藏悬浮按钮",
                "彻底关闭悬浮按钮",
                "立即处理一条排队胶囊",
                computerStatus,
                (cloudConfigured ? "腾讯转写：已配置 · " : "腾讯转写：待配置 · ")
                        + TranscriptionPolicyText.shortCondition(),
                "重新申请录音/文件权限"
        };
        new AlertDialog.Builder(this)
                .setTitle("设置")
                .setItems(options, (dialog, which) -> {
                    switch (which) {
                        case 0:
                            enableOverlay();
                            break;
                        case 1:
                            overlayAction(OverlayService.ACTION_HIDE);
                            break;
                        case 2:
                            overlayAction(OverlayService.ACTION_DISABLE);
                            break;
                        case 3:
                            TranscriptionScheduler.scheduleManual(this);
                            toast("已提交处理任务");
                            break;
                        case 4:
                            openDeviceSettings();
                            break;
                        case 5:
                            if (cloudConfigured) {
                                toast(TranscriptionPolicyText.automaticCondition());
                            } else {
                                openTencentConfigPicker();
                            }
                            break;
                        case 6:
                            requestRequiredPermissions();
                            break;
                        default:
                            break;
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void showPhoneSettings(String computerStatus) {
        String[] options = {
                "立即处理排队胶囊",
                computerStatus,
                (cloudConfigured ? "腾讯转写：已配置 · " : "腾讯转写：待配置 · ")
                        + TranscriptionPolicyText.shortCondition(),
                "重新申请录音/文件权限"
        };
        new AlertDialog.Builder(this)
                .setTitle("设置")
                .setItems(options, (dialog, which) -> {
                    switch (which) {
                        case 0:
                            TranscriptionScheduler.scheduleManual(this);
                            toast("已提交处理任务");
                            break;
                        case 1:
                            openDeviceSettings();
                            break;
                        case 2:
                            if (cloudConfigured) {
                                toast(TranscriptionPolicyText.automaticCondition());
                            } else {
                                openTencentConfigPicker();
                            }
                            break;
                        case 3:
                            requestRequiredPermissions();
                            break;
                        default:
                            break;
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void openDeviceSettings() {
        if (!DeviceRuntimeProfile.isLowPowerReader()) {
            try {
                startActivity(new Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS));
            } catch (Exception error) {
                startActivity(new Intent(Settings.ACTION_SETTINGS));
            }
            return;
        }
        try {
            Intent settings = new Intent("com.onyx.action.SETTING");
            settings.setPackage("com.onyx");
            startActivity(settings);
        } catch (Exception error) {
            startActivity(new Intent(Settings.ACTION_SETTINGS));
        }
    }

    private void openTencentConfigPicker() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("text/plain");
        startActivityForResult(picker, REQUEST_TENCENT_CONFIG);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_TENCENT_CONFIG
                || resultCode != RESULT_OK
                || data == null
                || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        io.execute(() -> {
            try (InputStream input = getContentResolver().openInputStream(uri);
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                if (input == null) throw new IOException("无法读取密钥文件");
                byte[] buffer = new byte[4096];
                int total = 0;
                int count;
                while ((count = input.read(buffer)) >= 0) {
                    total += count;
                    if (total > 64 * 1024) throw new IOException("密钥文件过大");
                    output.write(buffer, 0, count);
                }
                TencentAsrConfig.save(
                        this,
                        TencentAsrConfig.parse(
                                new String(output.toByteArray(), StandardCharsets.UTF_8)));
                cloudConfigured = true;
                TranscriptionScheduler.scheduleAutomatic(this);
                runOnUiThread(() -> {
                    toast("腾讯转写已配置，排队胶囊将自动处理");
                    refresh();
                });
            } catch (Exception error) {
                runOnUiThread(() -> toast("配置失败：" + error.getMessage()));
            }
        });
    }

    private String readProperty(String key) {
        try {
            Process process = Runtime.getRuntime().exec(
                    new String[]{"/system/bin/getprop", key});
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream()));
            String value = reader.readLine();
            process.waitFor();
            return value == null ? "" : value.trim();
        } catch (Exception error) {
            return "";
        }
    }

    private void enableOverlay() {
        if (!Settings.canDrawOverlays(this)) {
            Intent permission = new Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(permission);
            return;
        }
        Intent service = new Intent(this, OverlayService.class);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(service);
        else startService(service);
    }

    private void toggleInlineRecording() {
        if (DeviceRuntimeProfile.isLowPowerReader()) return;
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
            requestRequiredPermissions();
            return;
        }
        Intent service = new Intent(this, RecordingService.class);
        service.setAction(inlineRecording
                ? RecordingService.ACTION_STOP
                : RecordingService.ACTION_START);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(service);
        else startService(service);
    }

    private void overlayAction(String action) {
        Intent service = new Intent(this, OverlayService.class);
        service.setAction(action);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(service);
        else startService(service);
    }

    private void toggleSelected(String id) {
        if (!selected.add(id)) selected.remove(id);
        renderList();
    }

    private List<String> selectedIdsOrWarn() {
        if (selected.isEmpty()) {
            toast("请先长按选择胶囊");
            return null;
        }
        return new ArrayList<>(selected);
    }

    private void runStoreOperation(StoreOperation operation, String success) {
        io.execute(() -> {
            try {
                operation.run();
                runOnUiThread(() -> {
                    selected.clear();
                    toast(success);
                    refresh();
                });
            } catch (Exception error) {
                runOnUiThread(() -> toast("操作失败: " + error.getMessage()));
            }
        });
    }

    private void promptText(String title, String hint, TextResult callback) {
        EditText input = new EditText(this);
        input.setHint(hint);
        input.setSingleLine(true);
        int pad = dp(18);
        LinearLayout shell = new LinearLayout(this);
        shell.setPadding(pad, 0, pad, 0);
        shell.addView(input, lp(-1, -2));
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setView(shell)
                .setPositiveButton("确定", (dialog, which) -> callback.accept(input.getText().toString()))
                .setNegativeButton("取消", null)
                .show();
    }

    private TextView smallButton(String label, View.OnClickListener listener) {
        return ViewKit.button(this, label, listener);
    }

    private LinearLayout row() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    private LinearLayout.LayoutParams lp(int width, int height) {
        return new LinearLayout.LayoutParams(width, height);
    }

    private LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams value = new LinearLayout.LayoutParams(0, -1, 1f);
        value.setMargins(dp(2), dp(3), dp(2), dp(3));
        return value;
    }

    private int dp(int value) {
        return ViewKit.dp(this, value);
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private static Set<String> ids(List<CapsuleRecord> records) {
        HashSet<String> result = new HashSet<>();
        for (CapsuleRecord record : records) result.add(record.id);
        return result;
    }

    private interface StoreOperation {
        void run() throws Exception;
    }

    private interface TextResult {
        void accept(String value);
    }
}
