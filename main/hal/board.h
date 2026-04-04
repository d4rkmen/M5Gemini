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
    };

} // namespace HAL
