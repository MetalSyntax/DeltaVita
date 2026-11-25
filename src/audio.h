/**
 * @file audio.h
 * @brief Salida de audio: el núcleo escribe muestras a su frecuencia y un hilo las remuestrea a 48 kHz.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void vaudio_init(double core_rate);
void vaudio_set_rate(double core_rate);
/** frames estéreo intercalados (L, R) */
void vaudio_push(const int16_t *frames, size_t count);
void vaudio_set_muted(bool muted);
void vaudio_clear(void);
void vaudio_quit(void);
