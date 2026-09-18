# alarma-ya

Un botón de pánico para una casa.

Tocas un botón en el teléfono, un mensaje cruza internet y una sirena de 130 dB
suena en el pasillo. No hay aplicación que instalar ni servidor que mantener: una
página web publica un mensaje MQTT, un ESP32 montado en la pared está suscrito a
él, y un relé cierra.

Es deliberadamente pequeño. Tres piezas móviles, un tópico, dos mensajes.

---

## Cómo funciona

```
  PUBLICADORES                        BROKER                    SUSCRIPTOR

  web/index.html  --wss://host:8884--+
  (cualquier navegador)              |
                                     +-->  HiveMQ Cloud  --ssl://host:8883-->  ESP32
  atajo del teléfono --ssl://host:8883+     alarma-ya/comando                  + relé
  (app HTTP Shortcuts)                        "ON" / "OFF"                     + sirena

  credencial solo de publicación                    credencial solo de suscripción
```

El broker es lo único que los tres comparten, y lo único que autentica a alguien.
No hay un backend propio que escribir, desplegar ni mantener vivo — lo que
también significa que no hay nada propio que puedan vulnerar.

Conviene notar que aquí no hay cliente privilegiado. El panel web es un publicador
más, no *la* aplicación. Cualquier cosa capaz de publicar `ON` en
`alarma-ya/comando` dispara la sirena, y ese es exactamente el motivo por el que
hay un broker en el medio en lugar de un servidor propio: agregar una forma nueva
de activar la alarma es agregar un cliente, no desplegar código.

### Cómo se conecta cada uno de los tres clientes

El mismo broker, el mismo tópico, tres formas distintas de entrar — y confundir
los puertos es la manera más común de perder una tarde aquí.

| Cliente | Punto de conexión | Transporte |
|---|---|---|
| ESP32 | `8883` | MQTT sobre TLS, TCP directo |
| Atajo del teléfono | `ssl://host:8883` | MQTT sobre TLS, TCP directo |
| Panel web | `wss://host:8884/mqtt` | MQTT sobre WebSocket sobre TLS |

El navegador es el caso distinto, y no por gusto: **una página web no puede abrir
un socket TCP directo**, así que MQTT tiene que ir encapsulado en un WebSocket.
Esa es la única razón por la que el puerto `8884` aparece en este proyecto. Todo
lo demás usa el `8883`.

Todas las conexiones son TLS. HiveMQ Cloud no acepta texto plano, así que no
existe una variante sin cifrar a la que caer por descuido ni que olvidar activar.

El ESP32 además **verifica el certificado del broker** contra el paquete de
autoridades raíz que viene con el núcleo del ESP32 — la misma lista de confianza
que carga un navegador. El cifrado por sí solo no alcanzaría, y vale la pena
decir el motivo sin rodeos: las credenciales prueban el *dispositivo* ante
HiveMQ, y la validación del certificado es lo único que prueba *HiveMQ* ante el
dispositivo. Apuntan en direcciones opuestas. Sin la segunda, cualquier cosa
capaz de desviar el tráfico puede presentar un certificado propio, leer las
credenciales directamente del paquete CONNECT y, a partir de ahí, decidir qué
órdenes escucha la sirena — incluida ninguna, durante un robo, con la luz de
estado indicando «armado».

**Esa validación necesita un reloj real**, y esta es la parte con la que casi
todos tropiezan. Comprobar un certificado incluye preguntar si hoy cae entre sus
dos fechas, y un ESP32 recién encendido cree que es 1 de enero de 1970 — anterior
a cualquier certificado emitido. Por eso el firmware sincroniza la hora por NTP
antes de su primer handshake y se niega a intentar la conexión hasta que el reloj
sea creíble.

Lo que significa que el dispositivo necesita **NTP saliente (UDP 123)** en tu red,
no solo MQTT. Si la placa se queda en parpadeo lento para siempre y el registro
serie dice que el reloj no está sincronizado, eso es un puerto bloqueado, no un
broker roto.

Algunos detalles de la conexión del dispositivo, ya que es la que tiene que
sostenerse durante años:

- **El identificador de cliente** es `alarma-ya-<MAC de efuse>`, derivado del
  propio chip. Dos placas nunca pueden colisionar, y una colisión haría que el
  broker expulse la sesión más vieja en cada reconexión — dos dispositivos
  peleando por una sesión, desconectándose mutuamente, para siempre.
- **Keepalive de 60 s, tiempo de espera de socket de 30 s.** Ambos están subidos
  respecto a los valores por defecto de la biblioteca, que son lo bastante
  ajustados como para que un handshake TLS lento se lea como una conexión caída.
- **Sesión limpia.** Sin cola de mensajes pendientes, a propósito: una orden de
  pánico que llega diez minutos tarde es peor que una que no llega nunca.

### El contrato MQTT

Todo lo que los tres clientes tienen que acordar cabe en cuatro líneas:

- **Tópico:** `alarma-ya/comando`
- **Mensajes:** las cadenas literales `ON` y `OFF`. No se actúa sobre ninguna otra.
- **Retención:** tiene que ser `false`. Un `ON` retenido se lo reenvía el broker a
  cada nuevo suscriptor en el instante en que se conecta, así que la sirena se
  volvería a disparar en cada reconexión, para siempre. La retención es para
  estado; esto es una orden, y una orden es un evento que ya ocurrió.
- **QoS:** el dispositivo se suscribe con QoS 1. El panel publica hoy con QoS 0, y
  MQTT degrada en silencio al menor de los dos, así que la entrega actual es, en
  la práctica, como máximo una vez.

---

## Configurar HiveMQ Cloud

Necesitas un broker antes de que nada de esto funcione, y el plan gratuito sobra
para una casa. No hay nada que instalar ni que operar: creas un clúster, creas
credenciales, y ese es todo el backend.

### 1. Crear el clúster

Regístrate en [hivemq.cloud](https://www.hivemq.com/mqtt-cloud-broker/) y crea un
clúster gratuito. Cuando esté en marcha, abre **Cluster Details** y copia la
**URL**. Tiene la forma `algo.s1.eu.hivemq.cloud`.

Copia **solo el nombre de host** — sin `https://`, sin `ssl://`, sin puerto y sin
barra final. Esa misma cadena va en `MQTT_HOST` dentro de `secrets.h`, en el campo
*Broker* del panel web y en la URL del atajo del teléfono. Los tres clientes
apuntan al mismo lugar.

### 2. Crear las credenciales

Abre la pestaña **Access Management**. Una credencial ahí es un usuario, una
contraseña y un conjunto de permisos — y cada permiso es un filtro de tópico más
la actividad que habilita: publicar, suscribirse o ambas.

Crea **dos**, no un par compartido:

| Nombre sugerido | Permiso | Filtro de tópico | Va en |
|---|---|---|---|
| `alarma-ya-device` | **Solo suscripción** | `alarma-ya/comando` | `src/secrets.h` del ESP32 |
| `alarma-ya-trigger` | **Solo publicación** | `alarma-ya/comando` | panel web y atajo del teléfono, escrita en tiempo de uso |

Esa separación es el modelo de seguridad real de este proyecto, así que vale la
pena ser preciso sobre qué te da cada mitad:

| Si se filtra | Qué puede hacer alguien | Qué no puede hacer |
|---|---|---|
| la credencial del dispositivo | leer tus órdenes | enviar una — la sirena nunca suena |
| la credencial de disparo | hacer sonar tu sirena | escuchar nada, leer ningún tópico |

Ninguna de las dos es una llave maestra, y ese es todo el punto. La página web se
ejecuta en el navegador del visitante, así que cualquier cosa incrustada en ella
se puede leer con *ver código fuente* — que es exactamente el motivo por el que la
página no trae ninguna credencial y la pide en tiempo de uso. **La contención no
es el secreto, es el permiso asociado a la cuenta.**

Dos cosas que conviene hacer mientras estás ahí:

- Acota el filtro de tópico a `alarma-ya/comando` exacto. Un comodín como `#` se
  escribe más rápido y entrega el broker entero.
- Si más de un teléfono recibe la credencial de disparo, crea una credencial por
  dispositivo. Así perder un teléfono es revocar una credencial, y no volver a
  emparejar todo lo que tienes.

---

## Hardware

| Pieza | Especificación | Notas |
|---|---|---|
| Devkit ESP32 WROOM-32 | 30 pines, GPIO a 3.3 V | ~250 mA, picos cercanos a 400 mA al transmitir por WiFi |
| Módulo relé de 2 canales | SRD-05VDC-SL-C, bobina de 5 V, optoacopladores PC817, contactos 10 A a 30 VDC | **activo en BAJO** — ver más abajo |
| Sirena | 12 V DC, 20 W, 130 dB — 1.67 A nominales | |
| Fuente de 12 V | 2 A / 24 W | alimenta solo la sirena |
| Cargador USB | 1 A | alimenta solo el ESP32 |
| Resistencia | 10 kΩ | pull-up en GPIO26 — **obligatoria** |
| Diodo | 1N4007 | flyback sobre la sirena |
| Cable | AWG 20 para el lado de 12 V | los Dupont son AWG 26–28 y no soportan 1.67 A |

### Dos circuitos, un único punto de encuentro

Esta es la idea que vuelve obvio todo el resto del cableado: **aquí no hay un
circuito, hay dos**, y el relé es el único lugar donde se tocan.

El lado lógico trabaja a 3.3 V y unos pocos miliamperios, todo alimentado desde el
cargador USB a través del ESP32. El lado de potencia mueve 1.67 A a 12 V y nunca
se acerca a la placa. Dentro del relé los dos se encuentran sin conducirse: el
optoacoplador pasa la orden convertida en *luz*, y los contactos son metal que se
toca o no se toca.

No alimentes el ESP32 desde la fuente de 12 V, y no hagas pasar la corriente de la
sirena por la placa.

### Cableado

**Antes que nada, quita el jumper azul JD-VCC del módulo relé.**

Ese jumper une `VCC` con `JD-VCC`, alimentando el optoacoplador y la bobina desde
la misma línea. Los necesitas separados, y este es el motivo: el LED interno del
PC817 empieza a conducir alrededor de 1.2 V. Si alimentas `VCC` con 5 V mientras
el ESP32 pone `IN1` en ALTO a 3.3 V, quedan 1.7 V sobre ese LED — el relé se queda
pegado o se comporta de forma errática. Pon `VCC` a 3.3 V y un pin en ALTO
significa cero voltios sobre el LED, que es apagado de verdad. La bobina sigue
recibiendo sus 5 V por `JD-VCC`. Guarda el jumper; no lo tires.

Lado lógico:

| Desde | Hacia | Por qué |
|---|---|---|
| ESP32 `GPIO26` | relé `IN1` | la orden. Nivel BAJO dispara. |
| ESP32 `3V3` | relé `VCC` | referencia del optoacoplador. 3.3 V, **nunca 5 V** |
| ESP32 `GND` | relé `GND` (header de 4 pines) | masa lógica |
| ESP32 `GPIO26` | ESP32 `3V3`, a través de 10 kΩ | mantiene el pin en ALTO mientras flota durante el arranque |
| ESP32 `VIN` | relé `JD-VCC` | alimentación de la bobina, ~72 mA. `VIN` es el riel de 5 V del USB. |
| ESP32 `GND` | relé `GND` (header de 3 pines) | retorno de la bobina |

Lado de potencia:

| Desde | Hacia | Cable |
|---|---|---|
| Fuente de 12 V `+` | relé `COM` (CH1) | AWG 20 |
| relé `NO` (CH1) | sirena `+` | AWG 20 |
| Fuente de 12 V `−` | sirena `−` | AWG 20 |
| 1N4007 sobre la sirena, **franja hacia `+`** | | flyback |

`NO`, no `NC`. En reposo el contacto queda abierto y la sirena callada.

Los pines GPIO del ESP32 **no toleran 5 V** — la hoja de datos lo dice con esas
palabras. Meterle 5 V al GPIO26 lo destruye. Esa es toda la razón por la que `VCC`
va a 3.3 V.

### El relé es activo en BAJO — y en Wokwi es al revés

Este es el párrafo más importante de todo el archivo.

Los módulos de relé opto-aislados construidos sobre PC817 + SRD-05VDC cierran el
contacto cuando el pin de entrada se pone en **BAJO**, no en ALTO. Eso no se
dedujo, se midió en el banco con `src/relay_polarity.cpp`: el LED de IN1 enciende
y el relé hace clic mientras GPIO26 está en BAJO.

Por eso el firmware lleva:

```c
#define RELAY_ACTIVE_LOW 1
```

**El módulo relé de Wokwi es activo en ALTO.** Si simulas con esto en `1`, la
lógica se lee invertida en el simulador. Cámbialo a `0` para Wokwi, y vuelve a
ponerlo en `1` antes de grabar hardware real.

Si te equivocas en esto sobre hardware real, el fallo no es sutil: la sirena
empieza a sonar en el instante en que se alimenta el ESP32, y la orden `ON` la
*apaga*. Todo invertido, a 130 dB.

Y el software por sí solo no lo cubre. Entre el momento en que llega la
alimentación y el momento en que corre tu primera línea de código, GPIO26 queda
flotando. Esa ventana le corresponde al **pull-up de 10 kΩ**, y por eso no es
opcional.

### Comprobar la polaridad tú mismo

No te fíes de lo que dice este archivo — tu módulo puede no ser el mismo. Hay un
segundo entorno de PlatformIO que no hace más que responder esta pregunta, sin
WiFi ni MQTT de por medio:

```bash
pio run -e relay_polarity -t upload -t monitor
```

Conecta **solo** el lado lógico y el de la bobina. Sin 12 V y sin sirena. El pin
queda en ALTO durante 10 segundos y después alterna BAJO/ALTO cada 3 segundos.
Escucha el clic y lee la salida serie: la línea que se imprime mientras el relé
está cerrado te dice cuál es tu nivel activo.

---

## Firmware

### Configurar `secrets.h`

Todo lo que cambia entre instalaciones vive en un único archivo ignorado por git.

```bash
cp src/secrets.h.example src/secrets.h
```

Después completa:

| Definición | Qué es |
|---|---|
| `WIFI_AP1_SSID` / `WIFI_AP1_PASS` | tu red. Las ranuras 2 y 3 son opcionales — descomenta un par para registrarlo. |
| `MQTT_HOST` | el nombre de host de tu clúster de HiveMQ. Sin esquema, sin puerto y sin barra final. |
| `MQTT_USER` / `MQTT_PASS` | la credencial de solo suscripción |

Dos cosas que le cuestan horas a mucha gente:

- **La radio del ESP32 es solo de 2.4 GHz.** Un SSID de 5 GHz no va a asociarse
  nunca, por más correcta que sea la contraseña. Si tu router emite ambas bandas
  con el mismo nombre, la banda de 2.4 GHz igual tiene que estar habilitada.
- `WiFiMulti` elige una red **en el momento de conectarse y no hace roaming**.
  Escanea, se une a la más fuerte que realmente alcanza a ver, y se queda ahí
  hasta que el enlace se cae. Tener varios puntos de acceso significa «este
  firmware arranca en más de un lugar sin volver a grabarlo», no «te sigue por
  toda la casa».

`src/secrets.h` está ignorado por git. `.pio/` también, porque el `firmware.bin`
compilado lleva tus credenciales incrustadas.

### Compilar y grabar

```bash
pio run -e esp32dev                       # compilar
pio run -e esp32dev -t upload             # compilar y grabar
pio run -e esp32dev -t upload -t monitor  # grabar y ver el registro serie a 115200
```

`esp32dev` es el entorno por defecto, así que un `pio run` a secas hace lo mismo.
Solo se compila `src/main.cpp` — `build_src_filter` deja la prueba de banco fuera
de la compilación de producción.

Usa un cable micro-USB **de datos**. Los cables de solo carga no muestran ningún
puerto COM, y el fallo se ve exactamente igual que una placa muerta.

---

## El LED de estado

El LED azul integrado es la única lectura local que tiene este aparato, así que
vale la pena saber interpretarlo. Sin él no puedes distinguir una placa
arrancando de una placa armada, y una alarma que nadie puede confirmar que está
armada es una alarma en la que nadie confía.

| Patrón | Significado |
|---|---|
| **Parpadeo rápido** (~5 Hz) | buscando una red WiFi |
| **Parpadeo lento** (~1.2 Hz) | el WiFi está bien, el broker no responde |
| **Pulso breve** cada 3 s | conectado, suscrito, **armado** |
| **Encendido fijo** | la sirena está sonando ahora mismo |

El disparo tiene prioridad sobre todo lo demás: haga lo que haga la red, una
bocina sonando es el dato que necesitas primero.

El LED lo pinta un temporizador por hardware y no el `loop()`, y eso es una
corrección, no un adorno. `WiFiMulti::run()` bloquea unos nueve segundos mientras
escanea y se asocia. Cuando el patrón se repintaba desde `loop()`, una placa que
simplemente no encontraba su red se repintaba una vez cada nueve segundos — lo que
se lee como una **luz fija**. En este dispositivo una luz fija significa que la
sirena está sonando. Una placa que solo estaba perdida informaba el estado más
alarmante que tiene. A un temporizador no lo puede dejar sin tiempo una llamada
bloqueante.

---

## El panel web

`web/index.html` es un único archivo estático. Sin paso de compilación, sin nada
que instalar, sin servidor. Ábrelo desde el disco, o publícalo en cualquier
alojamiento estático.

La primera vez pide tres cosas — host del broker, usuario y contraseña — y, si
marcas la casilla, las recuerda en `localStorage` para que el siguiente toque sea
inmediato. Quien las valida es el broker: un CONNACK rechazado te devuelve al
formulario en lugar de reintentar con una contraseña que nunca va a funcionar.

**Disparo de un toque.** La página lee `?fire=on` de su propia URL y dispara en
cuanto la conexión está lista. Apunta una etiqueta NFC a
`https://tu-host/index.html?fire=on` y el toque pasa a ser toda la interacción. Si
la conexión todavía no está lista, la petición se retiene, no se pierde.

---

## Dispararlo desde el teléfono

El panel necesita un navegador, una carga de página y un saludo de conexión antes
de poder enviar nada. Un cliente MQTT dedicado en la pantalla de inicio se saltea
todo eso, y para un botón de pánico esa diferencia lo es todo.

Lo que está en uso diario aquí es
[HTTP Shortcuts](https://github.com/Waboodoo/HTTP-Shortcuts), una aplicación
Android de código abierto que pone botones de un toque en la pantalla de inicio.
El nombre es histórico — habla MQTT de forma nativa, y es lo que se usa en esta
instalación. Al crear el atajo, elige el tipo **MQTT**, no una petición HTTP.

Crea **dos** atajos, uno por orden:

| Campo | Atajo de ON | Atajo de OFF |
|---|---|---|
| URL | `ssl://tu-cluster.s1.eu.hivemq.cloud:8883` | igual |
| Tópico | `alarma-ya/comando` | igual |
| Mensaje | `ON` | `OFF` |
| Usuario / contraseña | la credencial de **solo publicación** | igual |

Tres cosas que te van a costar una tarde si las equivocas:

- **El esquema es `ssl://`**, no `mqtt://` ni `https://`. HiveMQ Cloud acepta
  únicamente TLS. Una conexión en texto plano se rechaza de plano en lugar de
  degradarse en silencio, así que el fallo se lee como un broker roto y no como
  una URL equivocada.
- **Puerto `8883`, no `8884`.** La aplicación habla MQTT directo, así que usa el
  mismo punto de conexión que el ESP32. El `8884` es el puerto de WebSocket y
  existe solo porque los navegadores no pueden hacer otra cosa.
- **Deja la retención apagada.** Un `ON` retenido se lo reenvía el broker a cada
  nuevo suscriptor, así que la sirena se volvería a disparar cada vez que el ESP32
  reconecta. El dispositivo lleva una protección de tres segundos justamente
  contra esto, pero esa protección es una red de seguridad, no un permiso para
  publicar órdenes retenidas.

Crea el atajo de OFF al mismo tiempo que el de ON, no después. Un disparador sin
su parada correspondiente te deja esperando los dos minutos del apagado
automático parado al lado de una bocina de 130 dB.

Probado solo en Android. Cualquier aplicación de iOS capaz de publicar un mensaje
MQTT debería funcionar — los cuatro campos de arriba son toda la información que
necesita el broker — pero nadie lo ha verificado aquí, así que queda escrito como
no probado y no como una afirmación.

---

## Seguridad

Tres protecciones independientes, porque una bocina de 130 dB se las gana.

**1. La sirena no puede sonar para siempre.** `SIREN_MAX_MS` son 2 minutos y se
aplica *en el dispositivo*, así que sobrevive a que el WiFi, el broker y el
teléfono mueran a la vez. Cada `ON` reinicia la cuenta, lo que lo convierte en un
interruptor de hombre muerto y no en un tope al evento: si la emergencia sigue,
vuelves a tocar y el reloj se reinicia. La comprobación corre al principio del
`loop()`, antes de cualquier trabajo de red, porque un enlace caído es
precisamente la situación en la que el `OFF` no va a llegar nunca.

**2. El `OFF` siempre se obedece.** Sin ventana de gracia y sin condiciones.
Negarse a parar nunca es el modo de fallo seguro.

**3. Un `ON` retenido no puede volver a disparar la sirena.** El panel nunca marca
la retención, pero el dispositivo no confía en el publicador: cualquier `ON` que
llegue dentro de los 3 segundos posteriores a suscribirse se trata como una
repetición del broker y se descarta.

Y en `setup()`, el nivel seguro se escribe en el pin **antes** de que `pinMode()`
lo convierta en salida. Al revés, el pin pasa un instante en su valor por defecto
de arranque — que es un bocinazo gratis de 130 dB.

### Ponerlo en marcha por primera vez

En este orden. Saltarse pasos aquí es como se quema el hardware.

1. **Mide la fuente de 12 V antes de enchufarle nada.** Punta roja al pin central
   del conector, negra al anillo exterior. Tiene que leer **+12 V**. Los
   adaptadores genéricos mienten en sus etiquetas, y la polaridad invertida
   destruye todo lo que tengan conectado.
2. **Confirma la polaridad del relé** con el entorno `relay_polarity` de más
   arriba.
3. **Monta el pull-up de 10 kΩ** antes de conectar cualquier carga.
4. **Prueba los contactos con algo inofensivo** — una lámpara de 12 V, un LED con
   su resistencia, o simplemente el multímetro en continuidad. Confirma que abre y
   cierra, y confirma que el apagado automático de `SIREN_MAX_MS` realmente se
   dispara.
5. **Recién ahora, la sirena** — con el 1N4007 ya montado, al aire libre o con
   protección auditiva, y con `SIREN_MAX_MS` puesto en un valor corto. 130 dB en
   una habitación cerrada causan daño en segundos.

---

## Simularlo

`diagram.json` y `wokwi.toml` hacen correr el simulador de Wokwi contra la
compilación real de PlatformIO (`.pio/build/esp32dev/firmware.*`), así que puedes
ejercitar la lógica MQTT, la reconexión y el apagado automático sin hardware sobre
la mesa.

Dos cosas que cambiar antes de que funcione:

- Pon `Wokwi-GUEST` / `""` en `secrets.h` — esa es la red virtual del simulador.
- Pon `RELAY_ACTIVE_LOW` en `0`, porque el relé de Wokwi es activo en ALTO.

Ten en cuenta que la sincronización NTP y la validación del certificado aplican en
el simulador igual que en hardware, y ninguna de las dos se ha vuelto a probar ahí
desde que se activó la validación. Si la placa simulada nunca alcanza el broker,
revisa la línea del reloj en el registro serie antes de suponer que el problema
está en otra parte.

Simula lo que tiene **estados**: firmware, protocolo, reconexión. Calcula lo que
tiene **números**: corriente, potencia, disipación. Mide lo que tiene
**tolerancias**: la fuente real, la sirena real, el relé real. Wokwi responde solo
la primera pregunta, y ningún simulador te va a decir si tu fuente aguanta.

---

## Limitaciones conocidas

Escritas en lugar de escondidas, porque estás a punto de confiar en este aparato.

- **El panel guarda su credencial en `localStorage` en texto plano.** Cualquiera
  con el dispositivo desbloqueado puede leerla. El alcance del daño está acotado
  por que esa credencial sea de solo publicación, pero es un intercambio real y se
  tomó a conciencia.
- **Las órdenes se entregan como máximo una vez — asumido, no pasado por alto.** El
  dispositivo se suscribe con QoS 1, todos los publicadores envían con QoS 0, y
  MQTT degrada al menor de los dos. Una publicación con QoS 0 se pierde en
  silencio cuando la conexión ya está muerta pero el cliente todavía no lo sabe, y
  un teléfono cambiando entre WiFi y datos móviles es la versión realista de eso.
  Se deja como caso extremo a propósito: la aplicación de atajos de Android no
  expone ningún ajuste de QoS, así que subirlo solo en el panel web haría que los
  dos disparadores se comportaran distinto sin volver más fiable al que la gente
  realmente usa.
- **No hay confirmación de entrega.** El panel informa que publicó, no que la
  sirena sonó. No existe un tópico de acuse de recibo. Junto con el punto
  anterior, trata un toque como una petición y no como una garantía — y confirma
  de oído.
- **El dispositivo depende de NTP.** La validación del certificado necesita un
  reloj real, así que una red que bloquee el UDP 123 saliente deja la alarma
  permanentemente desconectada.

---

## Licencia

MIT — ver [LICENSE](LICENSE). El texto de la licencia se conserva en inglés porque
es la redacción canónica de MIT y traducirla le quitaría validez.

Vale la pena leer su último párrafo antes de construir uno de estos. Esto maneja
12 V y una sirena de 130 dB, se publica TAL CUAL, sin garantía de ningún tipo, y
lo que conectes es tu responsabilidad.
