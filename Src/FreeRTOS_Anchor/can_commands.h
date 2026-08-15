#ifndef CAN_COMMANDS_H
#define CAN_COMMANDS_H

#include <mcp2515.h>
#include "can_frames.h"

// ==================== CAN Commands API ====================

class CANCommands {
private:
  MCP2515* mcp;
  
  // Helper function để gửi sequence of frames
  bool sendFrameSequence(const char* action, int frameCount,
                         const CANFrames::FrameData* frames,
                         const char** frameIDs) {
    int failed = 0;
    struct can_frame frame;

    for (int i = 0; i < frameCount; i++) {
      frame.can_id  = frames[i].id;
      frame.can_dlc = frames[i].dlc;
      memcpy(frame.data, frames[i].data, 8);
      if (mcp->sendMessage(&frame) != MCP2515::ERROR_OK) failed++;
      delay(50);
    }

    if (failed == 0)
      Serial.printf("%s OK\n", action);
    else
      Serial.printf("%s: %d/%d frames failed\n", action, failed, frameCount);

    return (failed == 0);
  }

public:
  CANCommands(MCP2515* mcpInstance) : mcp(mcpInstance) {}
  
  // API: Mở khóa xe (15 frames)
  bool unlockCar() {
    return sendFrameSequence(
      "UNLOCKING CAR",
      CANFrames::UNLOCK_FRAME_COUNT,
      CANFrames::UNLOCK_FRAMES,
      CANFrames::UNLOCK_FRAME_IDS
    );
  }
  
  // API: Khóa xe (16 frames)
  bool lockCar() {
    return sendFrameSequence(
      "LOCKING CAR",
      CANFrames::LOCK_FRAME_COUNT,
      CANFrames::LOCK_FRAMES,
      CANFrames::LOCK_FRAME_IDS
    );
  }
  
  // API: Khởi tạo MCP2515
  bool initialize(uint8_t csPin, CAN_SPEED bitrate, CAN_CLOCK clock) {
    mcp->reset();
    delay(100);

    if (mcp->setBitrate(bitrate, clock) != MCP2515::ERROR_OK) {
      Serial.println("CAN: setBitrate failed");
      return false;
    }
    if (mcp->setNormalMode() != MCP2515::ERROR_OK) {
      Serial.println("CAN: setNormalMode failed");
      return false;
    }

    Serial.println("CAN: initialized");
    return true;
  }
};

#endif // CAN_COMMANDS_H
