# XSDK Xenia (Fork Experimental para xsdk, vulkan4xenon y SDL-xenon)

<p align="center">
    <img height="180px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" alt="XSDK Xenia Logo" />
</p>

> [!WARNING]
> **PROYECTO MUY EXPERIMENTAL:**
> **XSDK Xenia** es un fork altamente experimental de [Xenia Canary](https://github.com/xenia-canary/xenia-canary). Su rendimiento **no se compara en absoluto con el de una consola Xbox 360 real**. Este fork está diseñado y optimizado específicamente para servir como **entorno de depuración rápida (rapid debugging) y pruebas de desarrollo** de homebrew, juegos y ports creados con el ecosistema de código abierto de Xbox 360:
> - SDK Abierto de Xbox 360: [xsdk](https://github.com/xenosworks/xsdk)
> - Capa gráfica Vulkan para Xenon: [vulkan4xenon](https://github.com/xenosworks/vulkan4xenon)
> - Biblioteca multimedia y entrada: [SDL-xenon](https://github.com/xenosworks/SDL-xenon)

---

## 🎯 ¿De qué trata este fork y para qué sirve?

En el desarrollo de ports y juegos modernos hacia la consola Xbox 360 utilizando herramientas de código abierto como `powerpc-xenon-gcc`, `newlib`, `pthread`, `vulkan4xenon` y `SDL-xenon`, los desarrolladores se encuentran con limitaciones severas en los emuladores tradicionales (como Xenia Canary upstream):
1. **Deadlocks atómicos de GCC:** Los emuladores asumen el patrón de generación de código de MSVC para operaciones atómicas (`lwarx`/`stwcx.`). GCC emite secuencias donde `lwarx` no siempre va acompañado de un `stwcx.` inmediato si una comparación falla, lo que en Xenia original generaba congelamientos de CPU al 100% y bloqueos permanentes.
2. **Falta de soporte de instrucciones PPC de cadena:** Rutinas estándar de `memcpy`/`memmove` y cadenas optimizadas en `newlib` y `xsdk` hacen uso de instrucciones string (`lswi`, `lswx`, `stswi`, `stswx`) y del conteo de bytes en el registro `XER[57:63]`, las cuales no estaban soportadas.
3. **Incompatibilidad con el pipeline de memoria de vulkan4xenon:** La capa Vulkan para Xenon transfiere datos entre GPU y CPU mediante `readback_resolve` y `readback_memexport`. Sin una coherencia bidireccional estricta en la memoria compartida, el buffer de comandos sobreescribía los datos con contenido obsoleto.
4. **Ausencia de registros del controlador de pantalla AVIVO:** Juegos y ports basados en `SDL-xenon` o framebuffer directo configuran registros del display controller AVIVO (`DC_LUT_AUTOFILL`, `D1MODE_V_COUNTER`, `D1CRTC_DOUBLE_BUFFER_CONTROL`) y contadores de ciclos GPU (`RBBM_PERFCOUNTER`), provocando que la aplicación se congele esperando sincronización vertical o finalización de swap.

**XSDK Xenia** soluciona todas estas incompatibilidades agregando una capa de compatibilidad y depuración rápida que permite iterar, compilar y probar ports de Xbox 360 directamente en PC en cuestión de segundos, sin necesidad de transferir archivos por FTP o reiniciar una consola física en cada cambio.

---

## 🛠️ ¿Qué implementa y cómo funciona?

### 1. CPU PowerPC y Backend JIT x64 (GCC, Atomics y Sincronización)
- **Gestión no bloqueante de reservas `lwarx`/`ldarx`/`stwcx.`/`stdcx.`:**
  Se reestructuró `EmitTryAcquireReservationHelper` y `EmitReservedStoreHelper`. En lugar de retener un bit global que bloquea a otros hilos si un `lwarx` es abandonado (comportamiento típico de `__atomic_compare_exchange` en GCC), la reserva se asocia individualmente al hilo (`cached_reserve_bit`). Si el hilo ejecuta otro `lwarx` sin un `stwcx.` previo, la reserva se renueva limpiamente sin provocar interrupciones ni deadlocks, y `stwcx.` realiza un intercambio atómico con `lock cmpxchg`.
- **Instrucciones String Word de PowerPC:**
  Implementación completa en el frontend PPC y HIR de `lswi`, `lswx`, `stswi` y `stswx`, junto con la persistencia del campo de conteo de bytes `XER[57:63]` (`xer_byte_count`) a través de `LoadXER` y `StoreXER`.
- **Instrucciones de estado FPU:**
  Implementación de `mtfsb0x` y `mtfsb1x` para manipulación de bits de estado flotante.
- **Determinismo Virtual contra ASLR:**
  Normalización de `PPCContext::TranslateVirtual` y `PPCContext::HostToGuestVirtual` con base fija (`0xE0000000`), garantizando que la memoria de hilos y contextos sea consistente independientemente de la aleatorización de direcciones del host.

### 2. GPU Xenos, AVIVO y Memoria Compartida (`vulkan4xenon`)
- **Coherencia CPU-GPU en operaciones de Readback:**
  Integración de `SharedMemory::MakeRangeValid` tras escrituras de CPU en `readback_resolve` y `readback_memexport` (tanto en el backend de Direct3D 12 como en Vulkan). Esto asegura que llamadas como `vkCmdCopyImageToBuffer` seguidas de `vkCmdCopyBufferToImage` reciban los datos reales y no texturas obsoletas.
- **Alineación de Flags por Defecto para vulkan4xenon:**
  Configuración por defecto optimizada para debug: `readback_resolve = "full"`, `readback_memexport = true`, `occlusion_query_viz = true` y `async_shader_compilation = false`.
- **Controlador de Pantalla AVIVO y Presentación Directa:**
  - Emulación de `DC_LUT_AUTOFILL` (registro `0x1928`) reportando bit 1 (`AUTOFILL_DONE`).
  - Emulación de sincronización vertical en `D1MODE_V_COUNTER` (`0x194C`).
  - Presentación directa de framebuffer en `D1CRTC_DOUBLE_BUFFER_CONTROL` (`0x183B` -> `IssueDirectSwap`) para ports que escriben directo al frontbuffer sin SwapChain XBox.
- **Contador de Rendimiento de 48 bits a 500 MHz (`RBBM_PERFCOUNTER`):**
  Emulación de `RBBM_PERFCOUNTER1_LOW` (`0x0399`) y `RBBM_PERFCOUNTER1_HI` (`0x039A`) mediante MMIO y paquetes PM4 `REG_TO_MEM`, empleado por `vulkan4xenon` para `vkGetQueryPoolResults` (TIMESTAMP).
- **Invalidación de Caché en `SURFACE_SYNC`:**
  Invalidación automática (`TracePlaybackWroteMemory`) ante solicitudes de coherencia `CP_COHER_CNTL`.

### 3. Kernel, VFS y Diagnóstico de Crashes para Ports
- **VFS POSIX (`rename` con sobrescritura):**
  Liberación explícita de descriptores en `HostPathFile::Destroy` antes de operaciones de borrado/sobrescritura, permitiendo que llamadas `rename()` de `newlib`/POSIX funcionen correctamente en sistemas de archivos Windows.
- **Vectored Exception Handlers (`RTL`):**
  Implementación de `RtlSetVectoredExceptionHandler` y `RtlClearVectoredExceptionHandler` en `xboxkrnl_rtl.cc`.
- **Sockets BSD y Red en XAM:**
  Implementación de `NetDll_getpeername`, resolución DNS real con `getaddrinfo` en `NetDll_XNetDnsLookup`, y ajuste endian en estructuras de sockets.
- **Herramientas de Diagnóstico Rápido:**
  - Habilitado `allow_game_relative_writes = true` por defecto.
  - Eliminado el cuadro de diálogo modal de bienvenida (`MessageBoxW`) para permitir ejecuciones automatizadas y headless por consola.
  - Flag `--exit_on_crash` para scripts de integración continua y pipelines de pruebas.
  - Volcado de excepciones enriquecido en consola con decodificación de registros PowerPC (`LR`, `CTR`, `CR`, `FPSCR`), cálculo de direcciones de fallo y **stack backtrace simbólico de PowerPC** recorriendo los marcos de pila guest.

---

## 🔗 Proyectos y Dependencias Compatibles

Este fork implementa y da soporte a las especificaciones y herramientas desarrolladas por la comunidad:
- **xsdk:** [https://github.com/xenosworks/xsdk](https://github.com/xenosworks/xsdk)
- **vulkan4xenon:** [https://github.com/xenosworks/vulkan4xenon](https://github.com/xenosworks/vulkan4xenon)
- **SDL-xenon:** [https://github.com/xenosworks/SDL-xenon](https://github.com/xenosworks/SDL-xenon)

---

## 💻 Compilación Local

### Requisitos en Windows:
- Visual Studio 2022 / Build Tools con soporte C++ (MSVC x64)
- [CMake](https://cmake.org/download/) (3.25 o superior)
- [Ninja](https://github.com/ninja-build/ninja/releases)
- [Python 3.10+](https://www.python.org/)
- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) (con `glslangValidator`, `spirv-tools` y `fxc.exe` del Windows SDK)

### Pasos de compilación:
```powershell
# Clonar el repositorio
git clone <URL_DE_TU_REPOSITORIO>
cd "XSDK Xenia"

# Compilar binarios de Release (xenia-app)
python xenia-build.py build --config=Release --target=xenia-app

# Ejecutar las suites de pruebas unitarias
.\build\bin\Windows\Release\xenia-cpu-tests.exe
.\build\bin\Windows\Release\xenia-kernel-tests.exe
.\build\bin\Windows\Release\xenia-vfs-tests.exe
```

---

## 🚀 Uso Rápido para Debug de Ports

Para ejecutar un ejecutable `.xex` generado con `xsdk` o `vulkan4xenon`:
```powershell
.\build\bin\Windows\Release\xenia_canary.exe "ruta\a\tu_juego.xex" --exit_on_crash
```

Si el juego o port utiliza variables de entorno de depuración (como `BT3_GS=gpu` para `vulkan4xenon` o `BT3_GS_THREAD=1` para colas asíncronas), el emulador gestionará los hilos y la sincronización GPU de forma fluida.

---

## 📄 Licencia

Este proyecto mantiene la licencia de código abierto original de Xenia (BSD 3-Clause). Para más detalles, consulta el archivo [LICENSE](LICENSE).
