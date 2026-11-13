# G923 PS3 Initializer (v0.1)

Aplicación de **prueba** para PS3 (PSL1GHT) que despierta un Logitech G923
(versión Xbox/PC, `046D:C26D`) y registra sus datos en un archivo de texto.
**No hace andar el volante en los juegos todavía.** Responde una sola pregunta:
¿puede la PS3 hablar con el G923?

## Estado honesto

- Escrito sin poder compilarlo con el toolchain de PS3 (solo se verificó la
  sintaxis en una PC y el decodificador con reportes reales de la captura).
- **No probado en una PS3.** Los puntos más inciertos están marcados abajo.

## Qué hace

1. Se registra como driver USB para `046D:C26D` y `046D:C26E`.
2. Con el volante en `C26D` manda `0F 00 01 01 42` (el mismo comando que G HUB).
3. El volante se reinicia y reaparece como `C26E` (HID). Abre el endpoint
   `0x81` y lee los reportes de 11 bytes.
4. Escribe `/dev_hdd0/tmp/g923_log.txt` con lo que ocurre y con volante,
   pedales y botones.

## Cómo compilar (WSL/Linux)

Necesita el toolchain `ps3dev` y `PSL1GHT` (open source) instalados:

```
export PS3DEV=/usr/local/ps3dev
export PSL1GHT=$PS3DEV
export PATH=$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin
make pkg
```

Genera `g923pkg.pkg`. Si `make` dice que no encuentra `-lusb`, hay que
reinstalar PSL1GHT con sus librerías (`ppu/sprx/libusb`).

## Cómo probar (PS3 con HEN activado a mano)

1. Activar HEN. Copiar el `.pkg` a un pendrive o a `/dev_hdd0/packages/` por FTP.
2. Instalarlo desde Package Manager > Install Package Files.
3. Abrir "G923 PS3 Initializer" desde el XMB. La pantalla queda negra: es normal.
4. Enchufar el G923 (con su fuente) o dejarlo enchufado. Esperar ~10 s.
   Mover el volante, los pedales y apretar botones.
5. Salir con el botón PS. Leer `/dev_hdd0/tmp/g923_log.txt` por FTP (webMAN).

## Qué buscar en el log

| Línea | Significa |
|-------|-----------|
| `[wake] attach` | la PS3 detectó el G923 en modo Xbox |
| `[wake] comando enviado: result=0 bytes=5` | el saludo salió bien |
| `[wake] detach` | el volante se reinicia (esperado) |
| `[hid] attach ... C26E` | reapareció como HID |
| líneas `volante=... Y=... Z=... Rz=...` | los datos llegan |

## Puntos inciertos

- Si la PS3 llama a `probe`/`attach` con el volante ya enchufado al abrir la app.
- Si un homebrew puede registrar un driver USB para ese ID (PS3xPAD lo hace como plugin).
- Qué pedal es cada eje (Y/Z/Rz). Los pedales vienen invertidos (reposo 255).

## Próximas versiones

0.2 pedales y volante calibrados · 0.3 botones/levas · 0.4 mando virtual
(`ioPadLddRegisterController`) · 0.5 force feedback.
