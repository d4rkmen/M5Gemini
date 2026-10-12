/**
 * @file board.h
 * @brief Hardware pin definitions and board configuration
 */
#pragma once

namespace HAL
{
    /**
     * @brief Board type
     */
    enum class BoardType
    {
        AUTO_DETECT,   // Auto-detect board type
        CARDPUTER,     // Original M5Cardputer with IOMatrix
        CARDPUTER_ADV, // M5Cardputer ADV with TCA8418
        STICK_S3,      // M5StickS3 with two buttons, M5PM1 PMIC and ES8311 codec
    };

    /**
     * @brief Boards whose mic and speaker go through the ES8311 codec
     */
    inline bool board_has_es8311(BoardType type)
    {
        return type == BoardType::CARDPUTER_ADV || type == BoardType::STICK_S3;
    }

} // namespace HAL
