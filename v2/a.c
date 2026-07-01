#include "raylib.h"
#include "raymath.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

#define MAX_HOTBAR 9
#define MAX_INVENTARIO_GRID 27  // <--- AGORA SÃO 27 SLOTS (3 linhas de 9 colunas)
#define MAX_TOTAL_SLOTS (MAX_HOTBAR + MAX_INVENTARIO_GRID) // 36 slots no total
#define MAX_MUNDO_QUALQUER 100
#define RESOLUCAO_MAX_MIDIA 1080

typedef struct {
    Texture2D textura;
    bool ehAnimado;
    char caminhoOriginal[520];
    bool ocupado;
} SlotItem;

typedef struct {
    Vector3 posicao;
    float largura;
    float altura;
    BoundingBox box;
    Texture2D textura;
    bool ativo;
    bool ehAnimado;
    bool pausado;
    char caminhoOriginal[520];
    FILE *videoPipe;
    unsigned char *videoBuffer;
    int videoLargura;
    int videoAltura;
    float tempoAcumulado;
    float tempoPorFrame;
    bool temAudio;
    Music audioStream;
    char caminhoAudioTmp[512];
} QuadroMundo;

// Limpa caminhos vindos do drag and drop
void LimparCaminhoLinux(char *destino, const char *origem) {
    const char *p = origem;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
        if (*p == '/') p++;
    }
    int i = 0;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        if (*p == '%' && *(p+1) == '2' && *(p+2) == '0') {
            destino[i++] = ' '; p += 3;
        } else {
            destino[i++] = *p; p++;
        }
    }
    destino[i] = '\0';
}

bool ValidarExtensaoMidia(const char *caminho, bool *ehVideo) {
    const char *ponto = strrchr(caminho, '.');
    if (!ponto) return false;
    char ext[10] = { 0 };
    for (int i = 0; ponto[i] != '\0' && i < 9; i++) ext[i] = tolower(ponto[i]);
    if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
        *ehVideo = false; return true;
    }
    if (strcmp(ext, ".gif") == 0 || strcmp(ext, ".mp4") == 0 || strcmp(ext, ".webm") == 0) {
        *ehVideo = true; return true;
    }
    return false;
}

bool ObterDimensoesVideo(const char *caminho, int *largura, int *altura, bool *temAudio) {
    char comando[1024];
    sprintf(comando, "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 \"%s\" > /tmp/video_specs.txt", caminho);
    *largura = 512; *altura = 512; *temAudio = false;

    if (system(comando) == 0) {
        FILE *f = fopen("/tmp/video_specs.txt", "r");
        if (f) {
            int w = 0, h = 0;
            if (fscanf(f, "%d,%d", &w, &h) == 2 || fscanf(f, "%d\n%d", &w, &h) == 2) {
                if (w > 0 && h > 0) {
                    if (w > RESOLUCAO_MAX_MIDIA || h > RESOLUCAO_MAX_MIDIA) {
                        float proporcao = (float)h / (float)w;
                        if (w > h) {
                            *largura = RESOLUCAO_MAX_MIDIA;
                            *altura = (int)(RESOLUCAO_MAX_MIDIA * proporcao);
                        } else {
                            *altura = RESOLUCAO_MAX_MIDIA;
                            *largura = (int)(RESOLUCAO_MAX_MIDIA / proporcao);
                        }
                    } else { *largura = w; *altura = h; }
                }
            }
            fclose(f);
        }
    }
    sprintf(comando, "ffprobe -v error -select_streams a -show_entries stream=codec_type -of csv=p=0 \"%s\" > /tmp/audio_specs.txt", caminho);
    if (system(comando) == 0) {
        FILE *f = fopen("/tmp/audio_specs.txt", "r");
        if (f) {
            char tipo[32] = {0};
            if (fscanf(f, "%31s", tipo) == 1 && strcmp(tipo, "audio") == 0) *temAudio = true;
            fclose(f);
        }
    }
    return true;
}

void IniciarPipeVideoEAudio(QuadroMundo *quadro, int idUnico) {
    char comando[2048];
    sprintf(comando, "ffmpeg -loglevel quiet -threads 1 -re -stream_loop -1 -i \"%s\" "
    "-preset veryfast -tune zerolatency -an -f rawvideo -pix_fmt rgba -s %dx%d -",
    quadro->caminhoOriginal, quadro->videoLargura, quadro->videoAltura);

    quadro->videoPipe = popen(comando, "r");
    quadro->videoBuffer = (unsigned char *)malloc(quadro->videoLargura * quadro->videoAltura * 4);
    quadro->tempoAcumulado = 0.0f;
    quadro->tempoPorFrame = 1.0f / 30.0f;
    quadro->pausado = false;

    if (quadro->temAudio) {
        sprintf(quadro->caminhoAudioTmp, "/tmp/parker_audio_%d.mp3", idUnico);
        char cmdAudio[1024];
        sprintf(cmdAudio, "ffmpeg -y -loglevel quiet -i \"%s\" -vn -acodec libmp3lame -q:a 2 %s",
                quadro->caminhoOriginal, quadro->caminhoAudioTmp);

        if (system(cmdAudio) == 0) {
            quadro->audioStream = LoadMusicStream(quadro->caminhoAudioTmp);
            quadro->audioStream.looping = true;
            PlayMusicStream(quadro->audioStream);
        } else {
            quadro->temAudio = false;
        }
    }
}

void FecharPipeVideoEAudio(QuadroMundo *quadro) {
    if (quadro->videoPipe) { pclose(quadro->videoPipe); quadro->videoPipe = NULL; }
    if (quadro->videoBuffer) { free(quadro->videoBuffer); quadro->videoBuffer = NULL; }
    if (quadro->temAudio) {
        UnloadMusicStream(quadro->audioStream);
        unlink(quadro->caminhoAudioTmp);
        quadro->temAudio = false;
    }
}

void AtuaizarVideoEAudioPipe(QuadroMundo *quadro) {
    if (!quadro->ativo || !quadro->ehAnimado) return;
    if (quadro->temAudio) {
        if (quadro->pausado || IsWindowHidden()) PauseMusicStream(quadro->audioStream);
        else { ResumeMusicStream(quadro->audioStream); UpdateMusicStream(quadro->audioStream); }
    }
    if (quadro->pausado) return;

    if (quadro->videoPipe && quadro->videoBuffer) {
        quadro->tempoAcumulado += GetFrameTime();
        if (quadro->tempoAcumulado >= quadro->tempoPorFrame) {
            quadro->tempoAcumulado = 0.0f;
            size_t tamanhoFrame = quadro->videoLargura * quadro->videoAltura * 4;
            int fd = fileno(quadro->videoPipe);
            fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
            struct timeval timeout = {0, 0};

            if (select(fd + 1, &set, NULL, NULL, &timeout) > 0) {
                size_t lidos = fread(quadro->videoBuffer, 1, tamanhoFrame, quadro->videoPipe);
                if (lidos == tamanhoFrame) UpdateTexture(quadro->textura, quadro->videoBuffer);
            }
        }
    }
}

int main(void) {
    int screenWidth = 1280;
    int screenHeight = 720;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(screenWidth, screenHeight, "Mundo 3D - Inventário estilo Minecraft (3x9)");
    InitAudioDevice();
    SetExitKey(KEY_NULL);

    Camera3D camera = { 0 };
    camera.position = (Vector3){ 0.0f, 2.0f, 5.0f };
    camera.target = (Vector3){ 0.0f, 2.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    DisableCursor();
    Vector2 cameraAngulos = { 0.0f, 0.0f };

    // --- SISTEMA DE INVENTÁRIO (Total: 36 slots) ---
    SlotItem todosSlots[MAX_TOTAL_SLOTS] = { 0 };
    int slotSelecionadoHotbar = 0;

    SlotItem itemSeguradoNoMouse = { 0 };
    bool segurandoItem = false;

    Image imgVazia = GenImageColor(80, 60, BLANK);
    ImageDrawRectangle(&imgVazia, 0, 0, 80, 60, DARKGRAY);
    Texture2D texSlotVazio = LoadTextureFromImage(imgVazia);
    UnloadImage(imgVazia);

    QuadroMundo quadrosNoMundo[MAX_MUNDO_QUALQUER] = { 0 };
    int quadroArrastandoIndex = -1;
    float distanciaArrasto = 0.0f;

    char statusTexto[256] = "Aperte 'E' para abrir o Inventario e arrastar multiplas midias!";
    bool jogar = true;
    bool jogoPausado = false;
    bool inventarioAberto = false;

    Color corFundoInventario = (Color){ 60, 60, 60, 255 };

    SetTargetFPS(60);

    while (jogar) {
        if (WindowShouldClose()) jogar = false;
        screenWidth = GetScreenWidth();
        screenHeight = GetScreenHeight();

        // --- CONTROLE DOS MENUS (ESC E E) ---
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (inventarioAberto) { inventarioAberto = false; DisableCursor(); }
            else {
                jogoPausado = !jogoPausado;
                if (jogoPausado) EnableCursor(); else DisableCursor();
            }
        }

        if (IsKeyPressed(KEY_E) && !jogoPausado) {
            inventarioAberto = !inventarioAberto;
            if (inventarioAberto) EnableCursor();
            else {
                DisableCursor();
                if (segurandoItem) {
                    for (int i = 0; i < MAX_TOTAL_SLOTS; i++) {
                        if (!todosSlots[i].ocupado) {
                            todosSlots[i] = itemSeguradoNoMouse;
                            break;
                        }
                    }
                    segurandoItem = false;
                }
            }
        }

        // --- DROP DE ARQUIVOS (MULTIPLO) ---
        if (IsFileDropped()) {
            FilePathList arquivosArrastados = LoadDroppedFiles();
            for (unsigned int i = 0; i < arquivosArrastados.count; i++) {
                char caminhoLimpo[512] = { 0 };
                LimparCaminhoLinux(caminhoLimpo, arquivosArrastados.paths[i]);

                bool ehVideo = false;
                if (ValidarExtensaoMidia(caminhoLimpo, &ehVideo)) {
                    char caminhoAbsoluto[520];
                    if (caminhoLimpo[0] != '/' && caminhoLimpo[0] != '\0') sprintf(caminhoAbsoluto, "/%s", caminhoLimpo);
                    else sprintf(caminhoAbsoluto, "%s", caminhoLimpo);

                    int slotVago = -1;
                    for (int s = 0; s < MAX_TOTAL_SLOTS; s++) {
                        if (!todosSlots[s].ocupado) { slotVago = s; break; }
                    }

                    if (slotVago != -1) {
                        Texture2D texPreview = { 0 };
                        char comando[1024];
                        sprintf(comando, "ffmpeg -y -loglevel quiet -i \"%s\" -vframes 1 /tmp/raylib_preview.png", caminhoAbsoluto);

                        if (system(comando) == 0) texPreview = LoadTexture("/tmp/raylib_preview.png");

                        if (texPreview.id > 0) {
                            todosSlots[slotVago].textura = texPreview;
                            todosSlots[slotVago].ehAnimado = ehVideo;
                            strcpy(todosSlots[slotVago].caminhoOriginal, caminhoAbsoluto);
                            todosSlots[slotVago].ocupado = true;
                            sprintf(statusTexto, "Multiplas midias carregadas com sucesso!");
                        }
                    }
                }
            }
            UnloadDroppedFiles(arquivosArrastados);
        }

        // --- LÓGICA DO JOGO ---
        if (!jogoPausado && !inventarioAberto) {
            if (IsKeyPressed(KEY_ONE)) slotSelecionadoHotbar = 0;
            if (IsKeyPressed(KEY_TWO)) slotSelecionadoHotbar = 1;
            if (IsKeyPressed(KEY_THREE)) slotSelecionadoHotbar = 2;
            if (IsKeyPressed(KEY_FOUR)) slotSelecionadoHotbar = 3;
            if (IsKeyPressed(KEY_FIVE)) slotSelecionadoHotbar = 4;
            if (IsKeyPressed(KEY_SIX)) slotSelecionadoHotbar = 5;
            if (IsKeyPressed(KEY_SEVEN)) slotSelecionadoHotbar = 6;
            if (IsKeyPressed(KEY_EIGHT)) slotSelecionadoHotbar = 7;
            if (IsKeyPressed(KEY_NINE)) slotSelecionadoHotbar = 8;

            float wheel = GetMouseWheelMove();
            if (wheel > 0) slotSelecionadoHotbar = (slotSelecionadoHotbar + 1) % MAX_HOTBAR;
            if (wheel < 0) slotSelecionadoHotbar = (slotSelecionadoHotbar - 1 + MAX_HOTBAR) % MAX_HOTBAR;

            float velocidadeAndar = 0.1f;
            float sensibilidadeMouse = 0.003f;
            Vector2 deltaMouse = GetMouseDelta();
            cameraAngulos.x -= deltaMouse.x * sensibilidadeMouse;
            cameraAngulos.y -= deltaMouse.y * sensibilidadeMouse;

            if (cameraAngulos.y > 89.0f * DEG2RAD) cameraAngulos.y = 89.0f * DEG2RAD;
            if (cameraAngulos.y < -89.0f * DEG2RAD) cameraAngulos.y = -89.0f * DEG2RAD;

            Vector3 direcaoOlhar = { cosf(cameraAngulos.y) * sinf(cameraAngulos.x), sinf(cameraAngulos.y), cosf(cameraAngulos.y) * cosf(cameraAngulos.x) };
            Vector3 direcaoDireita = { -cosf(cameraAngulos.x), 0.0f, sinf(cameraAngulos.x) };

            if (IsKeyDown(KEY_W)) { camera.position.x += direcaoOlhar.x * velocidadeAndar; camera.position.z += direcaoOlhar.z * velocidadeAndar; }
            if (IsKeyDown(KEY_S)) { camera.position.x -= direcaoOlhar.x * velocidadeAndar; camera.position.z -= direcaoOlhar.z * velocidadeAndar; }
            if (IsKeyDown(KEY_D)) { camera.position.x += direcaoDireita.x * velocidadeAndar; camera.position.z += direcaoDireita.z * velocidadeAndar; }
            if (IsKeyDown(KEY_A)) { camera.position.x -= direcaoDireita.x * velocidadeAndar; camera.position.z -= direcaoDireita.z * velocidadeAndar; }

            camera.target = Vector3Add(camera.position, direcaoOlhar);

            Ray raioVisao = GetMouseRay((Vector2){ screenWidth / 2.0f, screenHeight / 2.0f }, camera);
            int quadroMiradoIndex = -1;
            float menorDistancia = 999999.0f;

            for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
                if (!quadrosNoMundo[i].ativo) continue;
                RayCollision colisao = GetRayCollisionBox(raioVisao, quadrosNoMundo[i].box);
                if (colisao.hit && colisao.distance < menorDistancia) {
                    menorDistancia = colisao.distance;
                    quadroMiradoIndex = i;
                }
            }

            if (IsKeyDown(KEY_V)) {
                if (quadroArrastandoIndex == -1 && quadroMiradoIndex != -1) {
                    quadroArrastandoIndex = quadroMiradoIndex;
                    distanciaArrasto = menorDistancia;
                }
                if (quadroArrastandoIndex != -1) {
                    distanciaArrasto += GetMouseWheelMove() * 0.5f;
                    if (distanciaArrasto < 1.0f) distanciaArrasto = 1.0f;
                    Vector3 novaPosicao = Vector3Add(camera.position, Vector3Scale(direcaoOlhar, distanciaArrasto));
                    quadrosNoMundo[quadroArrastandoIndex].posicao = novaPosicao;

                    float larg = quadrosNoMundo[quadroArrastandoIndex].largura;
                    float alt = quadrosNoMundo[quadroArrastandoIndex].altura;
                    quadrosNoMundo[quadroArrastandoIndex].box = (BoundingBox){
                        (Vector3){ novaPosicao.x - larg/2, novaPosicao.y - alt/2, novaPosicao.z - 0.2f },
                        (Vector3){ novaPosicao.x + larg/2, novaPosicao.y + alt/2, novaPosicao.z + 0.2f }
                    };
                }
            } else {
                quadroArrastandoIndex = -1;
            }

            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && quadroMiradoIndex != -1) {
                if (quadrosNoMundo[quadroMiradoIndex].ehAnimado) {
                    quadrosNoMundo[quadroMiradoIndex].pausado = !quadrosNoMundo[quadroMiradoIndex].pausado;
                }
            }

            // MODIFICADO: Agora usa MOUSE_BUTTON_LEFT para colocar item no mapa
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && todosSlots[slotSelecionadoHotbar].ocupado && quadroArrastandoIndex == -1) {
                int slotMundo = -1;
                for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
                    if (!quadrosNoMundo[i].ativo) { slotMundo = i; break; }
                }

                if (slotMundo != -1) {
                    Vector3 posSpawn = Vector3Add(camera.position, Vector3Scale(direcaoOlhar, 3.0f));
                    float proporcao = (float)todosSlots[slotSelecionadoHotbar].textura.height / (float)todosSlots[slotSelecionadoHotbar].textura.width;
                    float larg = (proporcao > 1.0f) ? 1.5f : 2.5f;
                    float alt = larg * proporcao;

                    quadrosNoMundo[slotMundo].posicao = posSpawn;
                    quadrosNoMundo[slotMundo].largura = larg;
                    quadrosNoMundo[slotMundo].altura = alt;
                    quadrosNoMundo[slotMundo].ehAnimado = todosSlots[slotSelecionadoHotbar].ehAnimado;
                    strcpy(quadrosNoMundo[slotMundo].caminhoOriginal, todosSlots[slotSelecionadoHotbar].caminhoOriginal);

                    if (quadrosNoMundo[slotMundo].ehAnimado) {
                        ObterDimensoesVideo(quadrosNoMundo[slotMundo].caminhoOriginal, &quadrosNoMundo[slotMundo].videoLargura, &quadrosNoMundo[slotMundo].videoAltura, &quadrosNoMundo[slotMundo].temAudio);
                        Image img = GenImageColor(quadrosNoMundo[slotMundo].videoLargura, quadrosNoMundo[slotMundo].videoAltura, BLANK);
                        quadrosNoMundo[slotMundo].textura = LoadTextureFromImage(img);
                        UnloadImage(img);
                        IniciarPipeVideoEAudio(&quadrosNoMundo[slotMundo], slotMundo);
                    } else {
                        quadrosNoMundo[slotMundo].textura = todosSlots[slotSelecionadoHotbar].textura;
                    }

                    quadrosNoMundo[slotMundo].box = (BoundingBox){
                        (Vector3){ posSpawn.x - larg/2, posSpawn.y - alt/2, posSpawn.z - 0.2f },
                        (Vector3){ posSpawn.x + larg/2, posSpawn.y + alt/2, posSpawn.z + 0.2f }
                    };
                    quadrosNoMundo[slotMundo].ativo = true;
                    todosSlots[slotSelecionadoHotbar] = (SlotItem){ 0 };
                }
            }

            if (IsKeyPressed(KEY_Q) && quadroMiradoIndex != -1) {
                FecharPipeVideoEAudio(&quadrosNoMundo[quadroMiradoIndex]);
                if (quadrosNoMundo[quadroMiradoIndex].ehAnimado) UnloadTexture(quadrosNoMundo[quadroMiradoIndex].textura);
                quadrosNoMundo[quadroMiradoIndex].ativo = false;
            }
        }

        for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
            if (quadrosNoMundo[i].ativo && quadrosNoMundo[i].ehAnimado) AtuaizarVideoEAudioPipe(&quadrosNoMundo[i]);
        }

        // --- DESENHO EM TELA ---
        BeginDrawing();
        ClearBackground(DARKGRAY);

        BeginMode3D(camera);
        DrawGrid(30, 1.0f);
        for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
            if (!quadrosNoMundo[i].ativo) continue;
            DrawBillboard(camera, quadrosNoMundo[i].textura, quadrosNoMundo[i].posicao, quadrosNoMundo[i].altura, WHITE);
            if (i == quadroArrastandoIndex) DrawBoundingBox(quadrosNoMundo[i].box, BLUE);
        }
        EndMode3D();

        if (!jogoPausado && !inventarioAberto) DrawCircle(screenWidth / 2, screenHeight / 2, 4, RED);

        // --- RENDERIZAR INTERFACE DO INVENTÁRIO (MINECRAFT STYLE) ---
        int itemEspacamento = 90;
        int itemLargura = 80;
        int hotbarLargura = (MAX_HOTBAR * itemEspacamento) + 10;
        int hotbarX = (screenWidth / 2) - (hotbarLargura / 2);
        int hotbarY = screenHeight - 90;

        if (inventarioAberto) {
            DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.5f));

            // Ajustado a altura para comportar 3 linhas confortavelmente
            int invLargura = hotbarLargura;
            int invAltura = 450;
            int invX = hotbarX;
            int invY = (screenHeight / 2) - (invAltura / 2) - 10;

            DrawRectangle(invX, invY, invLargura, invAltura, Fade(corFundoInventario, 0.9f));
            DrawRectangleLines(invX, invY, invLargura, invAltura, LIGHTGRAY);
            DrawText("INVENTARIO DE MIDIAS (3x9)", invX + 15, invY + 15, 18, WHITE);

            // MODIFICADO: Renderização em Grade Matriz de 3 linhas por 9 colunas
            for (int i = 0; i < MAX_INVENTARIO_GRID; i++) {
                int linha = i / 9;
                int coluna = i % 9;

                int slotIdx = 9 + i; // Mapeamento correto dos índices (9 até 35)
                int sX = invX + 15 + (coluna * itemEspacamento);
                int sY = invY + 55 + (linha * 70); // 70px de deslocamento por linha

                Rectangle slotRect = { sX, sY, itemLargura, 60 };
                bool mouseSobre = CheckCollisionPointRec(GetMousePosition(), slotRect);
                DrawRectangleRec(slotRect, mouseSobre ? LIGHTGRAY : DARKGRAY);
                DrawRectangleLines(sX, sY, itemLargura, 60, mouseSobre ? GREEN : GRAY);

                if (todosSlots[slotIdx].ocupado) {
                    float esc = (todosSlots[slotIdx].textura.width > todosSlots[slotIdx].textura.height)? (80.0f/todosSlots[slotIdx].textura.width) : (60.0f/todosSlots[slotIdx].textura.height);
                    Vector2 pSlot = { sX + (80 - todosSlots[slotIdx].textura.width*esc)/2, sY + (60 - todosSlots[slotIdx].textura.height*esc)/2 };
                    DrawTextureEx(todosSlots[slotIdx].textura, pSlot, 0.0f, esc, WHITE);
                }

                if (mouseSobre && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    SlotItem temp = todosSlots[slotIdx];
                    todosSlots[slotIdx] = itemSeguradoNoMouse;
                    itemSeguradoNoMouse = temp;
                    segurandoItem = itemSeguradoNoMouse.ocupado;
                }
            }

            // Deslocado a Hotbar um pouco mais para baixo no painel para dar espaço à grade expandida
            DrawText("BARRA DE ACESSO RAPIDO (HOTBAR)", invX + 15, invY + 340, 14, LIGHTGRAY);
            for (int i = 0; i < MAX_HOTBAR; i++) {
                int sX = hotbarX + 10 + (i * itemEspacamento);
                int sY = invY + 370;

                Rectangle slotRect = { sX, sY, itemLargura, 60 };
                bool mouseSobre = CheckCollisionPointRec(GetMousePosition(), slotRect);
                DrawRectangleRec(slotRect, mouseSobre ? LIGHTGRAY : DARKGRAY);
                DrawRectangleLines(sX, sY, itemLargura, 60, mouseSobre ? GREEN : GRAY);

                if (todosSlots[i].ocupado) {
                    float esc = (todosSlots[i].textura.width > todosSlots[i].textura.height)? (80.0f/todosSlots[i].textura.width) : (60.0f/todosSlots[i].textura.height);
                    Vector2 pSlot = { sX + (80 - todosSlots[i].textura.width*esc)/2, sY + (60 - todosSlots[i].textura.height*esc)/2 };
                    DrawTextureEx(todosSlots[i].textura, pSlot, 0.0f, esc, WHITE);
                }

                if (mouseSobre && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    SlotItem temp = todosSlots[i];
                    todosSlots[i] = itemSeguradoNoMouse;
                    itemSeguradoNoMouse = temp;
                    segurandoItem = itemSeguradoNoMouse.ocupado;
                }
            }

            if (segurandoItem) {
                Vector2 mPos = GetMousePosition();
                float esc = (itemSeguradoNoMouse.textura.width > itemSeguradoNoMouse.textura.height)? (50.0f/itemSeguradoNoMouse.textura.width) : (40.0f/itemSeguradoNoMouse.textura.height);
                DrawTextureEx(itemSeguradoNoMouse.textura, (Vector2){mPos.x - 25, mPos.y - 20}, 0.0f, esc, WHITE);
            }
        }
        else if (!jogoPausado) {
            DrawRectangle(hotbarX, hotbarY - 10, hotbarLargura, 80, Fade(BLACK, 0.6f));
            for (int i = 0; i < MAX_HOTBAR; i++) {
                int slotX = hotbarX + 10 + (i * itemEspacamento);
                DrawRectangleLines(slotX, hotbarY, itemLargura, 60, (i == slotSelecionadoHotbar) ? GREEN : GRAY);

                if (todosSlots[i].ocupado) {
                    float escalaSlot = (todosSlots[i].textura.width > todosSlots[i].textura.height) ? (80.0f / todosSlots[i].textura.width) : (60.0f / todosSlots[i].textura.height);
                    Vector2 posSlot = { slotX + (80 - todosSlots[i].textura.width*escalaSlot)/2, hotbarY + (60 - todosSlots[i].textura.height*escalaSlot)/2 };
                    DrawTextureEx(todosSlots[i].textura, posSlot, 0.0f, escalaSlot, WHITE);
                    if (todosSlots[i].ehAnimado) DrawText("VÍDEO", slotX + 6, hotbarY + 47, 9, RED);
                } else {
                    DrawTexture(texSlotVazio, slotX, hotbarY, WHITE);
                }
                char numStr[2]; sprintf(numStr, "%d", i + 1);
                DrawText(numStr, slotX + 4, hotbarY + 4, 10, (i == slotSelecionadoHotbar) ? GREEN : LIGHTGRAY);
            }

            DrawText("WASD: Mover | 1-9: Hotbar | E: Abrir Inventario | Clique Esq: Colocar Midia | Segurar V: Mover objeto | DEL: Deletar", 20, 20, 16, WHITE);
            DrawText(statusTexto, 20, 45, 14, LIME);
        }

        // --- MENU DE PAUSA COM BOTÃO SAIR ---
        if (jogoPausado) {
            DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.75f));
            int mX = (screenWidth / 2) - 200;
            int mY = (screenHeight / 2) - 130;
            DrawRectangle(mX, mY, 400, 260, RAYWHITE);
            DrawRectangleLines(mX, mY, 400, 260, LIGHTGRAY);

            DrawText("JOGO PAUSADO", mX + 110, mY + 30, 24, BLACK);
            DrawText("Aperte ESC para Retornar", mX + 90, mY + 80, 16, DARKGRAY);

            Rectangle botaoSair = { mX + 100, mY + 160, 200, 50 };
            bool mouseNoBotao = CheckCollisionPointRec(GetMousePosition(), botaoSair);

            DrawRectangleRec(botaoSair, mouseNoBotao ? RED : LIGHTGRAY);
            DrawRectangleLinesEx(botaoSair, 2, mouseNoBotao ? MAROON : GRAY);
            DrawText("SAIR", botaoSair.x + 75, botaoSair.y + 15, 20, mouseNoBotao ? WHITE : BLACK);

            if (mouseNoBotao && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                jogar = false;
            }
        }

        EndDrawing();
    }

    for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
        if (quadrosNoMundo[i].ativo) {
            FecharPipeVideoEAudio(&quadrosNoMundo[i]);
            UnloadTexture(quadrosNoMundo[i].textura);
        }
    }
    for (int i = 0; i < MAX_TOTAL_SLOTS; i++) if (todosSlots[i].ocupado) UnloadTexture(todosSlots[i].textura);
    UnloadTexture(texSlotVazio);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
