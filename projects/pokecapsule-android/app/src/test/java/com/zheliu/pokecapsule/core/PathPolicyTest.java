package com.zheliu.pokecapsule.core;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public final class PathPolicyTest {
    @Test public void acceptsChineseAndTwoLevels() {
        assertTrue(PathPolicy.isValidFolderName("客户想法"));
        assertTrue(PathPolicy.isSafeRelativeFolder("工作/项目甲"));
        assertEquals("工作", PathPolicy.normalizeName("  工作  "));
    }

    @Test public void rejectsTraversalControlAndThirdLevel() {
        assertFalse(PathPolicy.isValidFolderName("."));
        assertFalse(PathPolicy.isValidFolderName(".."));
        assertFalse(PathPolicy.isValidFolderName("../外部"));
        assertFalse(PathPolicy.isValidFolderName("a/b"));
        assertFalse(PathPolicy.isValidFolderName("a\\b"));
        assertFalse(PathPolicy.isValidFolderName("坏\u0000名称"));
        assertFalse(PathPolicy.isSafeRelativeFolder("一/二/三"));
    }

    @Test public void protectsReservedFolders() {
        assertFalse(PathPolicy.isValidFolderName("Inbox"));
        assertFalse(PathPolicy.isValidFolderName("archive"));
        assertFalse(PathPolicy.isValidFolderName(".commands"));
    }

    @Test public void normalizesTagsWithoutHash() {
        assertEquals("灵感", PathPolicy.normalizeTag(" ## 灵感 "));
        assertTrue(PathPolicy.isValidTag("中文 标签"));
        assertFalse(PathPolicy.isValidTag("\n"));
    }
}
