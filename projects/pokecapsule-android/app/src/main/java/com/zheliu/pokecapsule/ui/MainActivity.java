package com.zheliu.pokecapsule.ui;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.zheliu.pokecapsule.core.PathPolicy;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.service.OverlayService;
import com.zheliu.pokecapsule.service.TranscriptionScheduler;
import com.zheliu.pokecapsule.storage.CapsuleStore;
import com.zheliu.pokecapsule.storage.PokePaths;
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
import java.io.InputStreamReader;

public final class MainActivity extends Activity {
    private static final int REQUEST_PERMISSIONS = 91;
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final CapsuleStore store = new CapsuleStore(new PokePaths());
    private final ArrayList<CapsuleRecord> visible = new ArrayList<>();
    private final Set<String> selected = new HashSet<>();
    private ArrayAdapter<String> adapter;
    private TextView heading;
    private TextView selectionBar;
    private String folderFilter = PathPolicy.INBOX;
    private String tagFilter;
    private boolean favoritesOnly;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(buildPage());
        requestRequiredPermissions();
    }

    @Override public void onResume() {
        super.onResume();
        refresh();
    }

    @Override public void onDestroy() {
        io.shutdownNow();
        super.onDestroy();
    }

    private View buildPage() {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(14), dp(10), dp(14), dp(10));
        page.setBackgroundColor(Color.WHITE);

        heading = ViewKit.text(this, "PokeCapsule · Inbox", 25, Typeface.BOLD);
        page.addView(heading, lp(-1, dp(46)));

        LinearLayout tabs = row();
        tabs.addView(smallButton("Inbox", v -> showInbox()), weight());
        tabs.addView(smallButton("目录", v -> showFolderChooser()), weight());
        tabs.addView(smallButton("标签", v -> showTagChooser()), weight());
        tabs.addView(smallButton("收藏", v -> showFavorites()), weight());
        tabs.addView(smallButton("设置", v -> showSettings()), weight());
        page.addView(tabs, lp(-1, dp(48)));

        ListView list = new ListView(this);
        list.setDividerHeight(dp(1));
        adapter = new ArrayAdapter<String>(
                this, android.R.layout.simple_list_item_1, new ArrayList<>()) {
            @Override public View getView(int position, View convertView, ViewGroup parent) {
                TextView text = (TextView) super.getView(position, convertView, parent);
                text.setTextSize(17);
                text.setTextColor(Color.BLACK);
                text.setGravity(Gravity.CENTER_VERTICAL);
                text.setMaxLines(4);
                text.setEllipsize(TextUtils.TruncateAt.END);
                text.setLineSpacing(dp(2), 1f);
                text.setPadding(dp(10), dp(8), dp(10), dp(8));
                text.setMinHeight(dp(64));
                return text;
            }
        };
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> {
            CapsuleRecord record = visible.get(position);
            if (selected.isEmpty()) {
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
        actions.addView(smallButton("移动", v -> chooseDestination(false)), weight());
        actions.addView(smallButton("复制", v -> chooseDestination(true)), weight());
        actions.addView(smallButton("标签", v -> promptTag()), weight());
        actions.addView(smallButton("收藏", v -> promptFavorite()), weight());
        actions.addView(smallButton("删除", v -> confirmDelete()), weight());
        page.addView(actions, lp(-1, dp(48)));
        return page;
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
                List<CapsuleRecord> all = store.scan();
                ArrayList<CapsuleRecord> filtered = new ArrayList<>();
                for (CapsuleRecord record : all) {
                    if (matches(record)) filtered.add(record);
                }
                runOnUiThread(() -> applyRecords(filtered));
            } catch (Exception error) {
                runOnUiThread(() -> toast("读取失败: " + error.getMessage()));
            }
        });
    }

    private boolean matches(CapsuleRecord record) {
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
        for (CapsuleRecord record : visible) {
            String prefix = selected.contains(record.id) ? "☑ " : "";
            adapter.add(prefix + record.displayLine());
        }
        adapter.notifyDataSetChanged();
        selectionBar.setText(selected.isEmpty()
                ? "长按胶囊开始多选"
                : "已选择 " + selected.size() + " 个胶囊 · 轻点继续选择");
    }

    private void showInbox() {
        folderFilter = PathPolicy.INBOX;
        tagFilter = null;
        favoritesOnly = false;
        selected.clear();
        heading.setText("PokeCapsule · Inbox");
        refresh();
    }

    private void showFavorites() {
        folderFilter = null;
        tagFilter = null;
        favoritesOnly = true;
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
                .setTitle("删除 " + ids.size() + " 个胶囊？")
                .setMessage("胶囊会先移入 PokeCapsule/.trash，不会立即抹除。")
                .setPositiveButton("删除", (dialog, which) ->
                        runStoreOperation(() -> store.deleteCapsules(ids), "已移入回收区"))
                .setNegativeButton("取消", null)
                .show();
    }

    private void showSettings() {
        boolean cloudConfigured = TencentAsrConfig.isConfigured(this);
        boolean adbEnabled = Settings.Global.getInt(
                getContentResolver(), Settings.Global.ADB_ENABLED, 0) == 1;
        String usbConfig = readProperty("sys.usb.config");
        String computerStatus = adbEnabled && usbConfig.contains("adb")
                ? "电脑管理：ADB 已就绪"
                : adbEnabled ? "电脑管理：调试已开，等待 USB"
                : "电脑管理：USB 调试未开启";
        String[] options = {
                Settings.canDrawOverlays(this) ? "开启悬浮按钮" : "授予悬浮窗权限",
                "临时隐藏悬浮按钮",
                "彻底关闭悬浮按钮",
                "立即处理一条排队胶囊",
                computerStatus,
                cloudConfigured ? "腾讯转写：已安全配置" : "腾讯转写：等待配置",
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
                            openOnyxSettings();
                            break;
                        case 6:
                            requestRequiredPermissions();
                            break;
                        default:
                            toast(cloudConfigured
                                    ? "腾讯转写已配置；插电并连接 Wi‑Fi 后自动处理"
                                    : "尚未配置腾讯语音识别");
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void openOnyxSettings() {
        try {
            Intent settings = new Intent("com.onyx.action.SETTING");
            settings.setPackage("com.onyx");
            startActivity(settings);
        } catch (Exception error) {
            startActivity(new Intent(Settings.ACTION_SETTINGS));
        }
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
