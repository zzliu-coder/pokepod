package com.zheliu.pokecapsule.transcription;

import java.io.File;
import java.io.IOException;

public interface WhisperAdapter {
    String transcribe(File model, File audio) throws IOException;
}
