# Temporária 7 — tela cheia, controles e apresentação

Abra **MK1-TEMPORARIA-7.exe** na raiz. Launcher e MK1 abrem em tela cheia.
Molduras metálicas vermelhas/douradas preenchem as margens sem cobrir a imagem.
SETTINGS > VIDEO permite desligar a moldura nas duas telas.

## Controles

Gamepads XInput/Xbox são detectados automaticamente, inclusive após conectar
com o programa aberto. Os dois primeiros recebem P1 e P2; desconectar um não
transfere o outro para outro jogador. Uma porta desconectada fica solta.

| Comando | Gamepad | Teclado P1 / P2 |
|---|---|---|
| Movimento | D-pad / analógico esquerdo | Setas / I K J L |
| Soco alto | X | A / F |
| Bloqueio | LB, RB, LT ou RT | S / G |
| Chute alto | Y | D / H |
| Soco baixo | A | Z / V |
| Chute baixo | B | X / B |
| Moeda original | Back/View | 5 / 6 |
| Iniciar | Start/Menu | 1 / 2 |

No launcher, A confirma, B volta, Y abre SETTINGS e LB/RB trocam abas.
As direções têm repetição de navegação; golpes usam os níveis elétricos
originais, sem macros ou turbo. Entrada de controle é consultada até a cada
4 ms; dispositivos ausentes são procurados a cada 250 ms. Esses intervalos
não são uma medida de latência total. Perder foco solta teclado e gamepad.

Dispositivos precisam oferecer XInput; não foi feita certificação física de
cada modelo nem acrescentado um driver para controles DirectInput/HID puros.
Somente MK1 está integrado; os outros gabinetes continuam indisponíveis.

## CRT e cores

VIDEO > SCANLINES mantém os modos anteriores e acrescenta **CRT / ENERGIA**:
feixe gaussiano normalizado em luz linear, largura por componente dependente
da intensidade, gamma de entrada/saída 2.2, ganho 1 e halation desligada.
O feixe distribui energia sem o ganho fixo 1.25–1.28 dos perfis BVM anteriores.
Picos SDR ainda podem saturar; isso não reproduz a faixa dinâmica de um CRT/HDR.

A referência é a documentação oficial do [RetroTINK](https://retrotink-llc.github.io/firmware/4k-experimental.html),
que descreve conservação/normalização de energia em luz linear e controle de
gamma. É uma implementação própria de passagem única, inspirada nesses
princípios; não copia nem certifica equivalência com o firmware RetroTINK.
Não há BFI, efeito temporal ou interpolação de movimento nesta versão.

CORES oferece Original (identidade), Vivas (saturação/contraste moderados) e
Quente (balanço RGB discreto). BRILHO oferece 80–120%; 100% é neutro.
Todos agem somente na saída, sem reescrever a paleta original das ROMs.
O raster completo continua 400×254 em 4:3; CRT mantém escala vertical inteira.

## VRR e alta frequência

DISPLAY > SINCRONIZAÇÃO: V-Sync, VRR/Adaptativo ou Imediato. O caminho
adaptativo usa flip-discard, consulta DXGI_FEATURE_PRESENT_ALLOW_TEARING e
preserva a flag na criação e no redimensionamento da swapchain. Quando
suportado, apresenta sem bloqueio VSync com ALLOW_TEARING. Sem suporte,
volta a VSync. VRR e Imediato compartilham a permissão DXGI; a política final
continua sob o controle do monitor/Windows/driver.

Ative G-Sync/FreeSync no monitor e no driver e use a taxa desejada no Windows.
A consulta DXGI confirma suporte ao caminho de apresentação; não confirma
que o monitor está efetivamente variando a frequência. Não alteramos o driver.

ALTA FREQUÊNCIA usa uma cadência independente baseada na taxa do desktop,
limitada a 360 Hz e aos slots disponíveis do DXGI, repetindo a última imagem
original quando necessário. Não cria novos quadros de animação. O guest
continua em 54.7069 Hz (18.27925 ms), com regras, golpes e áudio originais.
Para acompanhar diretamente cada quadro original em VRR, deixe a opção
desligada. Os testes de cadência cobrem 60, 120, 144 e 240 Hz.

## Frametime e latência

O produtor agora usa QPC e timer de alta resolução, em vez de depender apenas
dos blocos Windows de áudio de 20 ms para ditar a cadência de 18.27925 ms.
O produtor não espera GPU/Present. A swapchain tem fila máxima de um quadro e
um objeto waitable; o renderer adquire o slot antes de escolher o quadro mais
recente da caixa postal. Não acelera o jogo para recuperar atrasos do host.

AUDIO oferece a fila equilibrada (padrão) e a segura (140 ms, capacidade
160 ms). A reserva é a latência: a sonda `native/tests/audio_latency_probe.cpp`
mediu, do Push até a mixagem do Windows (loopback), reserva + 4 a 10 ms. Desde
26/11/2026 a equilibrada liga com a reserva anterior (80 ms, ou 140 ms no
estéreo Remasterizado de MK1/MK2) e, após 5 s de som, passa a ter 60 ms como
alvo, com pacotes de 10 ms nos três jogos; o regulador de cadência pelo nível
da fila (`mk5_timing.hpp`, agora também em MK2 e MK3) escoa o excedente sem
descartar amostras. Um underrun mantém ou devolve a reserva anterior até o fim
da sessão. Os 5 s cobrem a partida do DCS do MK3, que entrega o primeiro som
62 ms atrasado aos 3,0 s (amostra 144480): com 60 ms desde o início havia um
underrun nesse ponto em toda sessão. Medição desta máquina com a sonda
(produtor sintético, dispositivo real, um quadro 15 ms atrasado a cada 3 s):
mediana de 51 ms com 60 ms contra 84 ms (80 ms) e 146 ms (140 ms), sem
underrun; com pacotes de 20 ms os mesmos 60 ms tiveram underrun.
O modo diagnóstico `low` de 40 ms apresentou um underrun e pior regularidade
no ensaio final: fica fora do SETTINGS e não está qualificado para uso normal.
O recibo `check-game.json` preserva explicitamente esse resultado reprovado.
O PCM fornecido à fila é preservado integralmente, inclusive a cauda parcial.
O produtor de áudio das ROMs não muda. Desde 26/11/2026 a Remasterizada
reconstrói cada fonte a 48 kHz com sinc janelado (`native/audio/sinc_resampler.hpp`);
a Original mantém o núcleo de referência. Medição com
`native/tests/resampler_response_probe.cpp`: o núcleo de referência perde 1,6 dB
em 10 kHz e 6,3 dB em 15 kHz no DCS (3,8 e 12,7 dB no YM2151) e deixa imagens
a -18 dBc em 10 kHz; o sinc fica em 0,00 dB até 13 kHz no DCS e 20 kHz no
YM2151, com resíduo abaixo de -96 dBc. Na escuta de 26/11/2026 o usuário achou
essa reconstrução sozinha mais escura e pior que a Original.

### Masterização da Remasterizada (26/11/2026)

A Remasterizada passa a mixagem mono da placa, já a 48 kHz, por
`native/audio/studio_master.hpp` e sai em estéreo. A receita ("estúdio amplo")
foi escolhida pelo usuário ouvindo arquivos de comparação gerados das próprias
ROMs; métrica não substitui essa escuta. Etapas: gate espectral contra o piso
do DCS e replicação da banda acima do fim da banda do fluxo (só DCS, uma STFT
de 512 pontos, 10,7 ms de atraso), low shelf em 95 Hz, high shelf em 2,6 kHz,
-1 dB em 330 Hz, alargamento mid/side por ruído velvet acima de 250 Hz
(esquerdo + direito continua igual ao mono), sala curta por convolução
particionada (RT60 0,45 s, -15 dB) e compressor de barramento 2:1 relativo ao
nível do programa.

Medições nas ROMs do usuário: a música do DCS do MK2 termina em 9,35 kHz e a do
MK3 em 11,3 kHz; o DCS sai perto de -40 dBFS RMS (cerca de 11 bits), com piso
por amostra de -99,0 dB (MK2) e -93,3 dB (MK3). No MK1 a FM já chega a 20 kHz:
sem gate nem replicação; FM e DAC pelo sinc, vozes ADPCM pelo núcleo de
referência até terem a sua própria rodada de escuta. No MK3 a cadeia vale para
a mixagem inteira (ainda sem separação de música).

As cópias de banda são deslocadas em múltiplos de 4 bins (quadro/salto): outro
deslocamento gira a fase entre quadros sobrepostos e as cópias se cancelam
(medido: -8 dB na primeira cópia, nada na terceira). Com isso a saída do jogo
ficou a até 2 dB do protótipo aprovado em todas as bandas, nos sete trechos
(`build/native/audio-audition/scripts/game_render.py`). Os ganhos mestres da
Remasterizada foram reajustados para igualar a sonoridade BS.1770 da Original
nesses trechos de música (MK1 -0,45 dB, MK2 +0,31 dB, MK3 +15,6 dB); vozes e
golpes não entraram nessa medida. Saves: MK1 17, MK2 21, MK3 7.

Os recibos medem intervalos de submissão e chamadas Present, não o scanout
físico nem input-to-photon. O sistema inclui Windows, driver e monitor, que
não foram medidos com câmera de alta velocidade ou equipamento externo.

## Medição final desta máquina

Comparação com 600 quadros por sessão, 539 intervalos após aquecimento, VSync e
reserva 140 ms nos dois casos: o desvio dos intervalos do produtor caiu de
5,25929 ms para 0,252911 ms; a média final foi 18,2795 ms. O desvio entre chamadas
Present caiu de 5,49251 ms para 2,15352 ms. Esses números são do host, sem garantir
igualdade com os intervalos físicos do painel.

As quatro sessões finais qualificadas somaram 2.800 quadros originais, sem
underruns; o perfil Energia com alta frequência chegou a seleção paga e luta
original. A opção de alta frequência depende dos slots de apresentação do
Windows/driver e não certifica uma taxa física de 120/144/240 Hz nesta máquina.
O ensaio separado de 40 ms apresentou 1 underrun e continua registrado como
reprovado. Testes sintéticos da fila compararam integralmente as amostras
fornecidas à saída; isso não é comparação integral do áudio das ROMs com PCB.

## Evidências e armazenamento

`build/native/manual-temporary-7-20261025/` contém recibos JSON compactos:
contratos de entrada, persistência, GPU, processos, fluxo e sessões reais.
`delivery.json` identifica a compilação entregue e os testes finais.
Testes GPU comparam buffers completos em memória e liberam os pixels.
Não são gravados vídeos, PCM ou dumps. Objetos de compilação e perfis/logs
de ensaios usam árvores temporárias verificadas, removidas após os testes.
Produtos, ROMs e seed CMOS de 16 KB são preservados. Versões 1–6 ficam intactas.
