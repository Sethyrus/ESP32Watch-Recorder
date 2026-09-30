# ESP32Watch-Recorder

Grabadora de voz para la Waveshare **ESP32-S3-Touch-AMOLED-2.06**: graba con los dos micros de la placa en la microSD, lista las grabaciones, las reproduce por el altavoz y las borra.

Stack: `ESP-IDF 5.5.4` + `LVGL 9` + BSP Waveshare + [ESP32Watch-core](https://github.com/Sethyrus/ESP32Watch-core) (`watch_board` >= v0.5.0). Se abre desde [ESP32Watch-Launcher](https://github.com/Sethyrus/ESP32Watch-Launcher) (slot `ota_3`).

## Uso

| Pantalla | Contenido |
| --- | --- |
| Grabar | Tiempo, medidor de nivel en vivo, grabar/parar, pausa, lista de grabaciones (con el numero) y espacio libre en horas |
| Grabaciones | De la mas reciente a la mas antigua (las 50 ultimas): fecha, duracion y tamano |
| Reproductor | Play/pausa, -10 s / +10 s, volumen (5 niveles, se guarda) y borrar |
| Borrar | Confirmacion |

| Entrada | Accion |
| --- | --- |
| `BOOT` corto | Pulsar lo marcado con el aro (en Grabar, el boton de grabar) |
| `BOOT` mantenido | Pasar el aro al siguiente |
| `PWR` en Grabar | Sin grabar: volver al launcher. Grabando: apagar la pantalla, la grabacion sigue |
| `PWR` en el resto | Volver |
| Tactil | Todo, con la pantalla encendida |

Sin uso, la pantalla se oscurece y se apaga con el tiempo y el brillo de los Ajustes del launcher. Grabando o reproduciendo, el chip sigue despierto con la pantalla apagada (`watch_power_screen_off()`); si no, entra en light sleep. BOOT o PWR la encienden.

Las alarmas y el temporizador del launcher no suenan con la app abierta: al volver se muestran como "perdida".

Para pasar las grabaciones al ordenador: launcher > Ajustes > Conectar al ordenador (la microSD aparece como disco).

## Grabaciones

- `/sdcard/rec/AAAAMMDD_HHMMSS.wav` (hora del RTC), WAV PCM de 16 bits, mono, 16 kHz: 32 KB/s, ~115 MB por hora (una microSD de 2 GB da ~16 h).
- Los dos micros (ES7210, ganancia maxima de 37,5 dB) se mezclan a mono con +6 dB digitales.
- La cabecera se reescribe cada 10 s; si se corta la bateria, al abrir la app se repara con el tamano real del fichero.
- Menos de 1 s se descarta; con menos de 5 MB libres para sola y avisa.

## Codigo

| Componente | Contenido |
| --- | --- |
| `components/rec_audio` | Sin UI. `rec_audio.c`: micro -> buffer de 256 KB en PSRAM -> SD (tareas `rec_capture` y `rec_writer`) y SD -> altavoz (`rec_play`). `rec_wav.c`: cabecera WAV. `rec_files.c`: montar, listar, borrar, espacio libre |
| `components/rec_app` | UI. `rec_ui.c`: pila de pantallas, foco y tarea del sistema (copia recortada de `os_ui.c` del launcher). Una pantalla por fichero: `rec_record.c`, `rec_list.c`, `rec_player.c`. Fuentes Barlow e iconos Lucide en `fonts/` (licencias OFL e ISC) |

NVS: namespace `recorder` (volumen). Lee sin escribir `bright` y `timeout` del namespace `launcher`.

## Compilar y grabar

```sh
source "$HOME/.espressif/tools/activate_idf_v5.5.4.sh"
idf.py set-target esp32s3   # solo la primera vez
idf.py build
```

Para el reloj con el launcher: `./flash_all.sh recorder` en ESP32Watch-Launcher (la graba en `ota_3`). Sola, con `idf.py flash`, ocupa `factory` en lugar del launcher.

## Documentacion

Hardware, entorno y convenciones: [ESP32Watch-core/docs](https://github.com/Sethyrus/ESP32Watch-core/tree/main/docs).

## Licencia

MIT. Ver [LICENSE](LICENSE).
