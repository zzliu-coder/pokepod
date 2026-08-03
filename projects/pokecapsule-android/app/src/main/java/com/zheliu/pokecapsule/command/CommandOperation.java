package com.zheliu.pokecapsule.command;

public enum CommandOperation {
    BEGIN_MAINTENANCE("beginMaintenance"), END_MAINTENANCE("endMaintenance"),
    RESCAN("rescan"), IMPORT_TENCENT_CREDENTIALS("importTencentCredentials"),
    EXPORT_TENCENT_CREDENTIALS("exportTencentCredentials"),
    MOVE_CAPSULES("moveCapsules"), COPY_CAPSULES("copyCapsules"),
    DELETE_CAPSULES("deleteCapsules"), RESTORE_CAPSULES("restoreCapsules"),
    PURGE_CAPSULES("purgeCapsules"), SET_FAVORITE("setFavorite"),
    ADD_TAGS("addTags"), REMOVE_TAGS("removeTags"),
    COMMIT_FINAL_TEXT("commitFinalText"), COMMIT_IMPORT("commitImport"),
    COMMIT_CORRECTION("commitCorrection"), CREATE_FOLDER("createFolder"),
    RENAME_FOLDER("renameFolder"), DELETE_FOLDER("deleteFolderToInbox"),
    RENAME_TAG("renameTag"), MERGE_TAG("mergeTag"), DELETE_TAG("deleteTag"),
    REQUEUE_TRANSCRIPTION("requeueTranscription");

    public final String wireName;
    CommandOperation(String wireName) { this.wireName = wireName; }

    public static CommandOperation fromWire(String wireName) {
        for (CommandOperation operation : values()) {
            if (operation.wireName.equals(wireName)) return operation;
        }
        switch (wireName) {
            case "move": return MOVE_CAPSULES;
            case "copy": return COPY_CAPSULES;
            case "delete": return DELETE_CAPSULES;
            case "favorite": return SET_FAVORITE;
            case "tag_add": return ADD_TAGS;
            case "tag_remove": return REMOVE_TAGS;
            case "tag_rename": return RENAME_TAG;
            case "mkdir": return CREATE_FOLDER;
            case "rmdir": return DELETE_FOLDER;
            case "retryTranscription": return REQUEUE_TRANSCRIPTION;
            default: break;
        }
        throw new IllegalArgumentException("Unsupported command: " + wireName);
    }
}
