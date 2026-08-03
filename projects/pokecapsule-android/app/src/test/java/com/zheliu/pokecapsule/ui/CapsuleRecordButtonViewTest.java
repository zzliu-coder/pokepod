package com.zheliu.pokecapsule.ui;

import android.content.Context;
import android.util.AttributeSet;

import org.junit.Test;

import static org.junit.Assert.assertNotNull;

public final class CapsuleRecordButtonViewTest {
    @Test public void exposesConstructorsRequiredByXmlInflater() throws Exception {
        assertNotNull(CapsuleRecordButtonView.class.getConstructor(
                Context.class, AttributeSet.class));
        assertNotNull(CapsuleRecordButtonView.class.getConstructor(
                Context.class, AttributeSet.class, int.class));
    }
}
