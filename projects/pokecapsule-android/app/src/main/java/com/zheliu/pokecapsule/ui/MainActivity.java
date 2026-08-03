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
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.zheliu.pokecapsule.core.PathPolicy;
import com.zheliu.pokecapsule.core.LibraryScope;
import com.zheliu.pokecapsule.core.LibrarySort;
import com.zheliu.pokecapsule.R;
import com.zheliu.pokecapsule.model.CapsuleRecord;
import com.zheliu.pokecapsule.service.DeviceCapabilities;
import com.zheliu.pokecapsule.service.InternalBroadcasts;
import com.zheliu.pokecapsule.service.LibraryChangeNotifier;
import com.zheliu.pokecapsule.service.OverlayService;
import com.zheliu.pokecapsule.service.RecordingService;
import com.zheliu.pokecapsule.service.TranscriptionScheduler;
import com.zheliu.pokecapsule.service.TranscriptionPolicyText;
import com.zheliu.pokecapsule.storage.LibraryRepository;
import com.zheliu.pokecapsule.transcription.TencentAsrConfig;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public final class MainActivity extends Activity {
    public static final String EXTRA_OPEN_SETTINGS = "openSettings";
    private static final int REQUEST_PERMISSIONS = 91;
    private static final int REQUEST_TENCENT_CONFIG = 92;
    private LibraryRepository repository;
    private final ArrayList<CapsuleRecord> visible = new ArrayList<>();
    private final LibraryController controller = new LibraryController();
    private final Set<String> selected = controller.selection();
    private CapsuleListAdapter adapter;
    private TextView heading;
    private TextView libraryContext;
    private TextView statusStrip;
    private TextView menuButton;
    private TextView moreButton;
    private TextView selectionBar;
    private TextView moveAction;
    private TextView copyAction;
    private TextView tagAction;
    private TextView favoriteAction;
    private TextView deleteAction;
    private LinearLayout selectionActions;
    private LibraryMenuCoordinator menus;
    private String currentHeading = "收件箱";
    private int pendingCount;
    private int failedCount;
    private boolean selectionMode;
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
        repository = new LibraryRepository(this, message -> toast("操作失败: " + message));
        setContentView(buildPage());
        requestRequiredPermissions();
        if (getIntent().getBooleanExtra(EXTRA_OPEN_SETTINGS, false)) {
            getWindow().getDecorView().post(this::showSettings);
        }
    }

    @Override public void onResume() {
        super.onResume();
        cloudConfigured = TencentAsrConfig.isConfigured(this);
        refresh();
        if (cloudConfigured) TranscriptionScheduler.scheduleAutomatic(this);
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent.getBooleanExtra(EXTRA_OPEN_SETTINGS, false)) {
            getWindow().getDecorView().post(this::showSettings);
        }
    }

    @Override public void onStart() {
        super.onStart();
        IntentFilter filter = new IntentFilter(LibraryChangeNotifier.ACTION);
        filter.addAction(RecordingService.ACTION_STATE);
        InternalBroadcasts.register(this, libraryChangeReceiver, filter,
                LibraryChangeNotifier.INTERNAL_PERMISSION);
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
        repository.close();
        super.onDestroy();
    }

    private View buildPage() {
        DeviceCapabilities capabilities = DeviceCapabilities.current();
        ViewGroup windowContent = findViewById(android.R.id.content);
        FrameLayout screen = (FrameLayout) LayoutInflater.from(this)
                .inflate(R.layout.activity_library, windowContent, false);
        LinearLayout page = screen.findViewById(R.id.library_page);
        ListView list = screen.findViewById(R.id.capsule_list);
        heading = screen.findViewById(R.id.library_heading);
        libraryContext = screen.findViewById(R.id.library_context);
        statusStrip = screen.findViewById(R.id.library_status_strip);
        menuButton = screen.findViewById(R.id.library_menu);
        moreButton = screen.findViewById(R.id.library_more);
        selectionBar = screen.findViewById(R.id.selection_bar);
        selectionActions = screen.findViewById(R.id.selection_actions);
        moveAction = screen.findViewById(R.id.action_move);
        copyAction = screen.findViewById(R.id.action_copy);
        tagAction = screen.findViewById(R.id.action_tag);
        favoriteAction = screen.findViewById(R.id.action_favorite);
        deleteAction = screen.findViewById(R.id.action_delete);
        inlineRecordButton = screen.findViewById(R.id.inline_record_button);
        menus = new LibraryMenuCoordinator(this, screen);

        screen.setBackgroundColor(ViewKit.background(this));
        page.setBackgroundColor(ViewKit.background(this));
        heading.setTextColor(ViewKit.ink(this));
        libraryContext.setTextColor(ViewKit.secondary(this));
        statusStrip.setTextColor(ViewKit.ink(this));
        statusStrip.setBackgroundColor(ViewKit.accentSoft(this));
        selectionBar.setTextColor(ViewKit.secondary(this));
        menuButton.setTextColor(ViewKit.ink(this));
        moreButton.setTextColor(ViewKit.ink(this));
        menuButton.setOnClickListener(v -> {
            if (selectionMode) exitSelectionMode();
            else showLibraryDrawer();
        });
        moreButton.setOnClickListener(v -> {
            if (selectionMode) selectAllVisible();
            else showOverflowMenu();
        });
        statusStrip.setOnClickListener(v -> showFilterChooser());
        list.setDividerHeight(dp(1));
        list.setBackgroundColor(ViewKit.surface(this));
        adapter = new CapsuleListAdapter(this, selected);
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> {
            CapsuleRecord record = visible.get(position);
            if (!selectionMode && !controller.isTrash()) {
                Intent detail = new Intent(this, CapsuleDetailActivity.class);
                detail.putExtra("capsuleId", record.id);
                startActivity(detail);
            } else {
                toggleSelected(record.id);
            }
        });
        list.setOnItemLongClickListener((parent, view, position, id) -> {
            selectionMode = true;
            toggleSelected(visible.get(position).id);
            return true;
        });
        moveAction.setOnClickListener(v -> primaryMoveAction());
        copyAction.setOnClickListener(v -> promptCopyActions());
        tagAction.setOnClickListener(v -> promptTag());
        favoriteAction.setOnClickListener(v -> promptFavorite());
        deleteAction.setOnClickListener(v -> confirmDelete());
        for (TextView action : new TextView[]{moveAction, copyAction, tagAction,
                favoriteAction, deleteAction}) {
            action.setTextColor(ViewKit.ink(this));
        }

        if (!capabilities.inlineRecorder) {
            inlineRecordButton.setVisibility(View.GONE);
            return screen;
        }
        inlineRecordButton.setOnClickListener(v -> toggleInlineRecording());
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
        repository.load(snapshot -> {
            List<CapsuleRecord> source = controller.isTrash()
                    ? snapshot.trash : snapshot.active;
            applyRecords(
                    controller.query(source),
                    countScope(snapshot.active, LibraryScope.PENDING),
                    countScope(snapshot.active, LibraryScope.FAILED));
        });
    }

    private void applyRecords(List<CapsuleRecord> records, int pending, int failed) {
        visible.clear();
        visible.addAll(records);
        selected.retainAll(ids(records));
        pendingCount = pending;
        failedCount = failed;
        renderList();
    }

    private void renderList() {
        adapter.clear();
        adapter.addAll(visible);
        adapter.notifyDataSetChanged();
        if (selectionMode) {
            heading.setText(getString(R.string.selected_count_short, selected.size()));
            libraryContext.setText(R.string.select_more_hint);
            menuButton.setText("×");
            menuButton.setContentDescription("取消多选");
            moreButton.setText("全选");
            moreButton.setTextSize(14);
        } else {
            heading.setText(currentHeading);
            libraryContext.setText(getString(R.string.capsule_count, visible.size()));
            menuButton.setText(R.string.menu_symbol);
            menuButton.setContentDescription(getString(R.string.open_library));
            moreButton.setText(R.string.more_symbol);
            moreButton.setTextSize(28);
        }
        selectionBar.setText(selectionMode
                ? getString(R.string.selected_count, selected.size())
                : getString(R.string.select_hint));
        selectionActions.setVisibility(selectionMode ? View.VISIBLE : View.GONE);
        if (pendingCount > 0 || failedCount > 0) {
            if (pendingCount > 0 && failedCount > 0) {
                statusStrip.setText(getString(
                        R.string.status_pending_failed, pendingCount, failedCount));
            } else if (pendingCount > 0) {
                statusStrip.setText(getString(R.string.status_pending, pendingCount));
            } else {
                statusStrip.setText(getString(R.string.status_failed, failedCount));
            }
            statusStrip.setVisibility(View.VISIBLE);
        } else {
            statusStrip.setVisibility(View.GONE);
        }
        moveAction.setText(controller.isTrash() ? R.string.restore : R.string.move);
        copyAction.setText(controller.isTrash() ? R.string.copy_text : R.string.copy);
        tagAction.setEnabled(!controller.isTrash());
        favoriteAction.setEnabled(!controller.isTrash());
        deleteAction.setText(controller.isTrash() ? R.string.purge : R.string.delete);
    }

    private void showInbox() {
        controller.show(LibraryScope.INBOX);
        currentHeading = getString(R.string.inbox);
        refresh();
    }

    private void showFavorites() {
        controller.show(LibraryScope.FAVORITES);
        currentHeading = getString(R.string.scope_favorites);
        refresh();
    }

    private void showFolderChooser() {
        repository.loadFolders(folders -> {
            ArrayList<String> choices = new ArrayList<>();
            choices.add(PathPolicy.INBOX);
            choices.add(PathPolicy.ARCHIVE);
            choices.addAll(folders);
            new AlertDialog.Builder(this)
                    .setTitle("目录")
                    .setItems(choices.toArray(new String[0]), (dialog, which) -> {
                        String folder = choices.get(which);
                        controller.show(LibraryScope.folder(folder));
                        currentHeading = displayFolder(folder);
                        refresh();
                    })
                    .setPositiveButton("新建", (dialog, which) -> promptCreateFolder())
                    .setNeutralButton("删除目录", (dialog, which) -> promptDeleteFolder())
                    .setNegativeButton("取消", null)
                    .show();
        });
    }

    private void promptCreateFolder() {
        promptText("新建目录", "一级目录 或 一级/二级", value ->
                repository.createFolder(value, () -> operationCompleted("目录已创建")));
    }

    private void promptDeleteFolder() {
        promptText("删除目录", "输入完整目录路径", value ->
                new AlertDialog.Builder(this)
                        .setTitle("确认删除目录")
                        .setMessage("目录内全部胶囊会移回 Inbox。")
                        .setPositiveButton("确认", (d, w) ->
                                repository.deleteFolder(value,
                                        () -> operationCompleted("目录已删除，内容已回 Inbox")))
                        .setNegativeButton("取消", null)
                        .show());
    }

    private void showTagChooser() {
        repository.loadTags(tags -> new AlertDialog.Builder(this)
                .setTitle("标签")
                .setItems(tags.toArray(new String[0]), (dialog, which) -> {
                    String tag = tags.get(which);
                    controller.show(LibraryScope.tag(tag));
                    currentHeading = getString(R.string.scope_tag, tag);
                    refresh();
                })
                .setPositiveButton("改名/合并", (dialog, which) -> promptRenameTag())
                .setNegativeButton("取消", null)
                .show());
    }

    private void promptRenameTag() {
        promptText("原标签", "不含 #", oldTag ->
                promptText("新标签", "同名即合并", newTag ->
                        repository.renameTag(oldTag, newTag,
                                () -> operationCompleted("标签已更新"))));
    }

    private void chooseDestination(boolean copy) {
        List<CapsuleRecord> records = selectedRecordsOrWarn();
        if (records == null) return;
        repository.loadFolders(folders -> {
            ArrayList<String> destinations = new ArrayList<>();
            destinations.add(PathPolicy.INBOX);
            destinations.add(PathPolicy.ARCHIVE);
            destinations.addAll(folders);
            new AlertDialog.Builder(this)
                    .setTitle(copy ? "复制到" : "移动到")
                    .setItems(destinations.toArray(new String[0]), (dialog, which) -> {
                        Runnable success = () -> operationCompleted(copy ? "复制完成" : "移动完成");
                        if (copy) repository.copy(records, destinations.get(which), success);
                        else repository.move(records, destinations.get(which), success);
                    })
                    .setNegativeButton("取消", null)
                    .show();
        });
    }

    private void primaryMoveAction() {
        if (!controller.isTrash()) {
            chooseDestination(false);
            return;
        }
        List<CapsuleRecord> records = selectedRecordsOrWarn();
        if (records == null) return;
        repository.restore(records, () -> operationCompleted("已恢复胶囊"));
    }

    private void promptCopyActions() {
        List<String> ids = selectedIdsOrWarn();
        if (ids == null) return;
        if (controller.isTrash()) {
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
        List<CapsuleRecord> records = selectedRecordsOrWarn();
        if (records == null) return;
        promptText("批量标签", "输入标签，不含 #", tag ->
                new AlertDialog.Builder(this)
                        .setTitle("#" + PathPolicy.normalizeTag(tag))
                        .setItems(new String[]{"添加", "移除"}, (dialog, which) -> {
                            Runnable success = () -> operationCompleted("标签已更新");
                            if (which == 0) repository.addTag(records, tag, success);
                            else repository.removeTag(records, tag, success);
                        })
                        .show());
    }

    private void promptFavorite() {
        List<CapsuleRecord> records = selectedRecordsOrWarn();
        if (records == null) return;
        new AlertDialog.Builder(this)
                .setTitle("收藏")
                .setItems(new String[]{"设为收藏", "取消收藏"}, (dialog, which) ->
                        repository.setFavorite(records, which == 0,
                                () -> operationCompleted("收藏状态已更新")))
                .show();
    }

    private void confirmDelete() {
        List<CapsuleRecord> records = selectedRecordsOrWarn();
        if (records == null) return;
        new AlertDialog.Builder(this)
                .setTitle((controller.isTrash() ? "永久删除 " : "删除 ")
                        + records.size() + " 个胶囊？")
                .setMessage(controller.isTrash()
                        ? "录音和文字会永久删除，无法恢复。"
                        : "胶囊会进入回收站，可随时恢复。")
                .setPositiveButton(controller.isTrash() ? "永久删除" : "删除", (dialog, which) -> {
                    Runnable success = () -> operationCompleted(
                            controller.isTrash() ? "已永久删除" : "已移入回收站");
                    if (controller.isTrash()) repository.purge(records, success);
                    else repository.delete(records, success);
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void promptSearch() {
        promptText("全文搜索", "文字、标签或目录", value -> {
            controller.search(value);
            currentHeading = controller.search().isEmpty()
                    ? getString(R.string.scope_all)
                    : getString(R.string.scope_search, controller.search());
            refresh();
        });
    }

    private void showSmart(String filter) {
        boolean pending = "pending".equals(filter);
        controller.show(pending ? LibraryScope.PENDING : LibraryScope.FAILED);
        currentHeading = getString(pending ? R.string.scope_pending : R.string.scope_failed);
        refresh();
    }

    private void showFilterChooser() {
        String[] options = {"待转写", "转写失败", "回收站"};
        new AlertDialog.Builder(this)
                .setTitle("筛选胶囊")
                .setItems(options, (dialog, which) -> {
                    if (which == 0) showSmart("pending");
                    else if (which == 1) showSmart("failed");
                    else showTrash();
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showTrash() {
        controller.show(LibraryScope.TRASH);
        currentHeading = getString(R.string.scope_trash);
        refresh();
    }

    private void showLibraryDrawer() {
        repository.loadMenu(data -> {
                List<CapsuleRecord> active = data.snapshot.active;
                List<CapsuleRecord> trash = data.snapshot.trash;
                ArrayList<LibraryMenuCoordinator.Entry> entries = new ArrayList<>();
                entries.add(LibraryMenuCoordinator.Entry.section("胶囊"));
                entries.add(scopeEntry("收件箱", active, LibraryScope.INBOX, this::showInbox));
                entries.add(scopeEntry("全部胶囊", active, LibraryScope.ALL, () -> {
                    controller.show(LibraryScope.ALL);
                    currentHeading = getString(R.string.scope_all);
                    refresh();
                }));
                entries.add(scopeEntry("收藏", active, LibraryScope.FAVORITES, this::showFavorites));
                entries.add(scopeEntry("待转写", active, LibraryScope.PENDING,
                        () -> showSmart("pending")));
                entries.add(scopeEntry("转写失败", active, LibraryScope.FAILED,
                        () -> showSmart("failed")));
                entries.add(LibraryMenuCoordinator.Entry.divider());
                entries.add(LibraryMenuCoordinator.Entry.section("目录", this::showFolderChooser));
                entries.add(scopeEntry("归档", active, LibraryScope.folder(PathPolicy.ARCHIVE), () -> {
                    controller.show(LibraryScope.folder(PathPolicy.ARCHIVE));
                    currentHeading = "归档";
                    refresh();
                }));
                for (String folder : data.folders) {
                    LibraryScope scope = LibraryScope.folder(folder);
                    entries.add(LibraryMenuCoordinator.Entry.item(
                            displayFolder(folder), String.valueOf(countScope(active, scope)),
                            folder.contains("/"), controller.scope().equals(scope), () -> {
                                controller.show(scope);
                                currentHeading = displayFolder(folder);
                                refresh();
                            }));
                }
                entries.add(LibraryMenuCoordinator.Entry.divider());
                entries.add(LibraryMenuCoordinator.Entry.section("标签", this::showTagChooser));
                for (String tag : data.tags) {
                    LibraryScope scope = LibraryScope.tag(tag);
                    entries.add(LibraryMenuCoordinator.Entry.item(
                            "#" + tag, String.valueOf(countScope(active, scope)), false,
                            controller.scope().equals(scope), () -> {
                                controller.show(scope);
                                currentHeading = getString(R.string.scope_tag, tag);
                                refresh();
                            }));
                }
                entries.add(LibraryMenuCoordinator.Entry.divider());
                entries.add(LibraryMenuCoordinator.Entry.item(
                        "回收站", String.valueOf(trash.size()), false,
                        controller.isTrash(), this::showTrash));
                menus.showDrawer(deviceLabel(), entries,
                        this::promptCreateFolder, this::showSettings);
        });
    }

    private void showOverflowMenu() {
        String order = controller.sort() == LibrarySort.NEWEST_FIRST
                ? "排序：最新在前" : "排序：最旧在前";
        ArrayList<LibraryMenuCoordinator.Entry> entries = new ArrayList<>();
        entries.add(LibraryMenuCoordinator.Entry.item("⌕  搜索当前清单", "", false, false,
                this::promptSearch));
        entries.add(LibraryMenuCoordinator.Entry.item("⇅  " + order, "", false, false, () -> {
            controller.toggleSort();
            refresh();
        }));
        entries.add(LibraryMenuCoordinator.Entry.item("≡  筛选状态", "", false, false,
                this::showFilterChooser));
        entries.add(LibraryMenuCoordinator.Entry.item("✓  批量选择", "", false, false, () -> {
            selectionMode = true;
            renderList();
        }));
        menus.showOverflow(entries);
    }

    @Override public void onBackPressed() {
        if (menus != null && menus.closeTransientSurface()) return;
        if (selectionMode) {
            exitSelectionMode();
            return;
        }
        super.onBackPressed();
    }

    private LibraryMenuCoordinator.Entry scopeEntry(String label,
            List<CapsuleRecord> records, LibraryScope scope, Runnable action) {
        return LibraryMenuCoordinator.Entry.item(label,
                String.valueOf(countScope(records, scope)), false,
                controller.scope().equals(scope), action);
    }

    private static int countScope(List<CapsuleRecord> records, LibraryScope scope) {
        int count = 0;
        for (CapsuleRecord record : records) if (scope.includes(record)) count++;
        return count;
    }

    private String displayFolder(String folder) {
        if (PathPolicy.INBOX.equals(folder)) return "收件箱";
        if (PathPolicy.ARCHIVE.equals(folder)) return "归档";
        int separator = folder.lastIndexOf('/');
        return separator >= 0 ? folder.substring(separator + 1) : folder;
    }

    private String deviceLabel() {
        if (DeviceCapabilities.current().eink) return "Poke3";
        String model = Build.MODEL == null ? "Android" : Build.MODEL.trim();
        return model.isEmpty() ? "Android" : model;
    }

    private void exitSelectionMode() {
        selectionMode = false;
        selected.clear();
        renderList();
    }

    private void selectAllVisible() {
        selected.addAll(ids(visible));
        renderList();
    }

    private void showSettings() {
        startActivity(new Intent(this, SettingsActivity.class));
    }

    private void toggleInlineRecording() {
        if (!DeviceCapabilities.current().inlineRecorder) return;
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
        controller.toggleSelection(id);
        renderList();
    }

    private List<String> selectedIdsOrWarn() {
        if (selected.isEmpty()) {
            toast("请先长按选择胶囊");
            return null;
        }
        return new ArrayList<>(selected);
    }

    private List<CapsuleRecord> selectedRecordsOrWarn() {
        if (selected.isEmpty()) {
            toast("请先长按选择胶囊");
            return null;
        }
        ArrayList<CapsuleRecord> records = new ArrayList<>();
        for (CapsuleRecord record : visible) {
            if (selected.contains(record.id)) records.add(record);
        }
        if (records.size() != selected.size()) {
            toast("列表已经变化，请重新选择");
            exitSelectionMode();
            return null;
        }
        return records;
    }

    private void operationCompleted(String success) {
        selected.clear();
        selectionMode = false;
        toast(success);
        refresh();
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
        return ViewKit.quietButton(this, label, listener);
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

    private interface TextResult {
        void accept(String value);
    }
}
