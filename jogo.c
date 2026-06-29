#include "raylib.h"
#include "raymath.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

// --- CONFIGURAÇÕES DO JOGO ---
#define MAX_INVENTARIO 9
#define MAX_MUNDO_QUALQUER 100
#define RESOLUCAO_MAX_MIDIA 1080

typedef struct {
    Vector3 posicao;
    float largura;
    float altura;
    BoundingBox box;
    Texture2D textura;
    bool ativo;

    // Controle de Mídia via Único Pipe Combinado
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
    Music audioStream;            // <--- NOVO: Controle de áudio nativo pelo Raylib
    char caminhoAudioTmp[512];    // <--- NOVO: Guarda o caminho do arquivo de áudio temporário
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

// Identifica se a extensão é válida
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

// Obtém as dimensões e presença de áudio com suporte a Alta Resolução
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

// Inicia o Pipe aplicando o preset de Alta Fidelidade (veryfast) - AGORA APENAS VÍDEO
void IniciarPipeVideoEAudio(QuadroMundo *quadro, int idUnico) {
    char comando[2048];

    // O ffmpeg agora processa APENAS o vídeo (-an remove o áudio do pipeline)
    sprintf(comando, "ffmpeg -loglevel quiet -threads 1 -re -stream_loop -1 -i \"%s\" "
    "-preset veryfast -tune zerolatency -an -f rawvideo -pix_fmt rgba -s %dx%d -",
    quadro->caminhoOriginal, quadro->videoLargura, quadro->videoAltura);

    quadro->videoPipe = popen(comando, "r");
    quadro->videoBuffer = (unsigned char *)malloc(quadro->videoLargura * quadro->videoAltura * 4);
    quadro->tempoAcumulado = 0.0f;
    quadro->tempoPorFrame = 1.0f / 30.0f;
    quadro->pausado = false;

    // Tratamento nativo do Áudio com Raylib se o arquivo contiver som
    if (quadro->temAudio) {
        sprintf(quadro->caminhoAudioTmp, "/tmp/parker_audio_%d.mp3", idUnico);

        // Extrai o áudio em background rapidamente para um arquivo temporário
        char cmdAudio[1024];
        sprintf(cmdAudio, "ffmpeg -y -loglevel quiet -i \"%s\" -vn -acodec libmp3lame -q:a 2 %s",
                quadro->caminhoOriginal, quadro->caminhoAudioTmp);

        if (system(cmdAudio) == 0) {
            quadro->audioStream = LoadMusicStream(quadro->caminhoAudioTmp);
            quadro->audioStream.looping = true;
            PlayMusicStream(quadro->audioStream);
        } else {
            quadro->temAudio = false; // Falha na extração desativa o som
        }
    }
}

void FecharPipeVideoEAudio(QuadroMundo *quadro) {
    if (quadro->videoPipe) {
        pclose(quadro->videoPipe);
        quadro->videoPipe = NULL;
    }
    if (quadro->videoBuffer) {
        free(quadro->videoBuffer);
        quadro->videoBuffer = NULL;
    }
    if (quadro->temAudio) {
        UnloadMusicStream(quadro->audioStream);
        unlink(quadro->caminhoAudioTmp); // Exclui o arquivo temporário de áudio do sistema
        quadro->temAudio = false;
    }
}

void AtuaizarVideoEAudioPipe(QuadroMundo *quadro) {
    if (!quadro->ativo || !quadro->ehAnimado) return;

    // Gerenciamento e atualização do Áudio via Raylib (Evita estouros quando minimizado)
    if (quadro->temAudio) {
        if (quadro->pausado || IsWindowHidden()) {
            PauseMusicStream(quadro->audioStream);
        } else {
            ResumeMusicStream(quadro->audioStream);
            UpdateMusicStream(quadro->audioStream);
        }
    }

    if (quadro->pausado) return;

    if (quadro->videoPipe && quadro->videoBuffer) {
        quadro->tempoAcumulado += GetFrameTime();

        if (quadro->tempoAcumulado >= quadro->tempoPorFrame) {
            quadro->tempoAcumulado = 0.0f;
            size_t tamanhoFrame = quadro->videoLargura * quadro->videoAltura * 4;
            int fd = fileno(quadro->videoPipe);

            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);

            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;

            if (select(fd + 1, &set, NULL, NULL, &timeout) > 0) {
                size_t lidos = fread(quadro->videoBuffer, 1, tamanhoFrame, quadro->videoPipe);

                if (lidos == tamanhoFrame) {
                    UpdateTexture(quadro->textura, quadro->videoBuffer);
                }
                else if (lidos > 0) {
                    size_t faltam = tamanhoFrame - lidos;
                    unsigned char *descarte = (unsigned char *)malloc(faltam);
                    if (descarte) {
                        fread(descarte, 1, faltam, quadro->videoPipe);
                        free(descarte);
                    }
                }
            }
        }
    }
}

int main(void) {
    int screenWidth = 1280;
    int screenHeight = 720;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(screenWidth, screenHeight, "Parkerspace C - Alta Qualidade + Hotbar 9 Slots");

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

    Texture2D inventario[MAX_INVENTARIO] = { 0 };
    bool inventarioEhAnimado[MAX_INVENTARIO] = { false };
    char inventarioCaminhos[MAX_INVENTARIO][520] = { 0 };
    int slotSelecionado = 0;

    Image imgVazia = GenImageColor(80, 60, BLANK);
    ImageDrawRectangle(&imgVazia, 0, 0, 80, 60, DARKGRAY);
    Texture2D texSlotVazio = LoadTextureFromImage(imgVazia);
    UnloadImage(imgVazia);

    QuadroMundo quadrosNoMundo[MAX_MUNDO_QUALQUER] = { 0 };
    int quadroFocadoIndex = -1;
    int quadroArrastandoIndex = -1;
    float distanciaArrasto = 0.0f;

    char statusTexto[256] = "Arraste Mídias (PNG, JPG, GIF, MP4, WEBM) para a Hotbar!";
    bool jogar = true;

    float inspecaoZoom = 1.0f;
    Vector2 inspecaoPan = { 0.0f, 0.0f };

    SetTargetFPS(60);

    while (jogar) {
        if (WindowShouldClose()) jogar = false;
        screenWidth = GetScreenWidth();
        screenHeight = GetScreenHeight();

        if (quadroFocadoIndex == -1 && quadroArrastandoIndex == -1) {
            if (IsKeyPressed(KEY_ONE)) slotSelecionado = 0;
            if (IsKeyPressed(KEY_TWO)) slotSelecionado = 1;
            if (IsKeyPressed(KEY_THREE)) slotSelecionado = 2;
            if (IsKeyPressed(KEY_FOUR)) slotSelecionado = 3;
            if (IsKeyPressed(KEY_FIVE)) slotSelecionado = 4;
            if (IsKeyPressed(KEY_SIX)) slotSelecionado = 5;
            if (IsKeyPressed(KEY_SEVEN)) slotSelecionado = 6;
            if (IsKeyPressed(KEY_EIGHT)) slotSelecionado = 7;
            if (IsKeyPressed(KEY_NINE)) slotSelecionado = 8;

            float wheel = GetMouseWheelMove();
            if (wheel > 0) slotSelecionado = (slotSelecionado + 1) % MAX_INVENTARIO;
            if (wheel < 0) slotSelecionado = (slotSelecionado - 1 + MAX_INVENTARIO) % MAX_INVENTARIO;
        }

        if (quadroFocadoIndex == -1) {
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
        } else {
            float wheelInspecao = GetMouseWheelMove();
            inspecaoZoom += wheelInspecao * 0.1f;
            if (inspecaoZoom < 0.2f) inspecaoZoom = 0.2f;
            if (inspecaoZoom > 5.0f) inspecaoZoom = 5.0f;

            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                Vector2 deltaMousePan = GetMouseDelta();
                inspecaoPan.x += deltaMousePan.x; inspecaoPan.y += deltaMousePan.y;
            }
            if (IsKeyPressed(KEY_R)) { inspecaoZoom = 1.0f; inspecaoPan = (Vector2){ 0.0f, 0.0f }; }
        }

        for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
            if (quadrosNoMundo[i].ativo && quadrosNoMundo[i].ehAnimado) {
                AtuaizarVideoEAudioPipe(&quadrosNoMundo[i]);
            }
        }

        if (IsFileDropped()) {
            FilePathList arquivosArrastados = LoadDroppedFiles();
            unsigned int arquivoIdx = 0;

            for (int s = 0; s < MAX_INVENTARIO; s++) {
                int slotVerificar = (slotSelecionado + s) % MAX_INVENTARIO;
                if (arquivoIdx >= arquivosArrastados.count) break;

                if (inventario[slotVerificar].id == 0) {
                    char caminhoLimpo[512] = { 0 };
                    LimparCaminhoLinux(caminhoLimpo, arquivosArrastados.paths[arquivoIdx]);

                    bool ehVideo = false;
                    if (ValidarExtensaoMidia(caminhoLimpo, &ehVideo)) {
                        char caminhoAbsoluto[520];
                        if (caminhoLimpo[0] != '/' && caminhoLimpo[0] != '\0') sprintf(caminhoAbsoluto, "/%s", caminhoLimpo);
                        else sprintf(caminhoAbsoluto, "%s", caminhoLimpo);

                        Texture2D texPreview = { 0 };
                        char comando[1024];
                        sprintf(comando, "ffmpeg -y -loglevel quiet -i \"%s\" -vframes 1 /tmp/raylib_preview.png", caminhoAbsoluto);

                        if (system(comando) == 0) {
                            texPreview = LoadTexture("/tmp/raylib_preview.png");
                        }

                        if (texPreview.id > 0) {
                            inventario[slotVerificar] = texPreview;
                            inventarioEhAnimado[slotVerificar] = ehVideo;
                            strcpy(inventarioCaminhos[slotVerificar], caminhoAbsoluto);
                            sprintf(statusTexto, "Mídia guardada no Slot %d!", slotVerificar + 1);
                            arquivoIdx++;
                        } else {
                            sprintf(statusTexto, "Erro ao gerar prévia da mídia.");
                            arquivoIdx++;
                        }
                    } else {
                        sprintf(statusTexto, "Formato não aceito!");
                        arquivoIdx++;
                    }
                }
            }
            UnloadDroppedFiles(arquivosArrastados);
        }

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

        // --- LÓGICA DE MOVER / ARRASTAR COM A TECLA 'V' ---
        if (IsKeyDown(KEY_V) && quadroFocadoIndex == -1) {
            if (quadroArrastandoIndex == -1 && quadroMiradoIndex != -1) {
                quadroArrastandoIndex = quadroMiradoIndex;
                distanciaArrasto = menorDistancia;
                sprintf(statusTexto, "Movendo objeto... Solte V para fixar.");
            }

            if (quadroArrastandoIndex != -1) {
                Vector3 direcaoOlhar = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
                distanciaArrasto += GetMouseWheelMove() * 0.5f;
                if (distanciaArrasto < 1.0f) distanciaArrasto = 1.0f;

                Vector3 novaPosicao = Vector3Add(camera.position, Vector3Scale(direcaoOlhar, distanciaArrasto));
                quadrosNoMundo[quadroArrastandoIndex].posicao = novaPosicao;

                float larg = quadrosNoMundo[quadroArrastandoIndex].largura;
                float alt = quadrosNoMundo[quadroArrastandoIndex].altura;
                float esp = 0.5f;
                quadrosNoMundo[quadroArrastandoIndex].box = (BoundingBox){
                    (Vector3){ novaPosicao.x - larg/2, novaPosicao.y - alt/2, novaPosicao.z - esp/2 },
                    (Vector3){ novaPosicao.x + larg/2, novaPosicao.y + alt/2, novaPosicao.z + esp/2 }
                };
            }
        } else {
            if (quadroArrastandoIndex != -1) {
                quadroArrastandoIndex = -1;
                sprintf(statusTexto, "Objeto reposicionado!");
            }
        }

        // --- LÓGICA DE PAUSAR/DESPAUSAR COM O CLIQUE DIREITO (COM ÁUDIO SINCRONIZADO) ---
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && quadroMiradoIndex != -1 && quadroFocadoIndex == -1 && quadroArrastandoIndex == -1) {
            if (quadrosNoMundo[quadroMiradoIndex].ehAnimado) {
                quadrosNoMundo[quadroMiradoIndex].pausado = !quadrosNoMundo[quadroMiradoIndex].pausado;

                if (quadrosNoMundo[quadroMiradoIndex].pausado) {
                    sprintf(statusTexto, "Vídeo e Áudio Pausados!");
                } else {
                    sprintf(statusTexto, "Vídeo e Áudio Retomados!");
                }
            }
        }

        // Colocar objeto no mundo (Tecla E)
        if (IsKeyPressed(KEY_E) && quadroFocadoIndex == -1 && quadroArrastandoIndex == -1) {
            if (inventario[slotSelecionado].id > 0) {
                int slotMundoDisponivel = -1;
                for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
                    if (!quadrosNoMundo[i].ativo) { slotMundoDisponivel = i; break; }
                }

                if (slotMundoDisponivel != -1) {
                    Vector3 direcaoOlhar = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
                    Vector3 posicaoSpawn = Vector3Add(camera.position, Vector3Scale(direcaoOlhar, 3.0f));

                    float proporcaoOrig = (float)inventario[slotSelecionado].height / (float)inventario[slotSelecionado].width;
                    float larg = (proporcaoOrig > 1.0f) ? 1.5f : 2.5f;
                    float alt = larg * proporcaoOrig;
                    float esp = 0.5f;

                    quadrosNoMundo[slotMundoDisponivel].posicao = posicaoSpawn;
                    quadrosNoMundo[slotMundoDisponivel].largura = larg;
                    quadrosNoMundo[slotMundoDisponivel].altura = alt;
                    quadrosNoMundo[slotMundoDisponivel].ehAnimado = inventarioEhAnimado[slotSelecionado];
                    strcpy(quadrosNoMundo[slotMundoDisponivel].caminhoOriginal, inventarioCaminhos[slotSelecionado]);

                    if (quadrosNoMundo[slotMundoDisponivel].ehAnimado) {
                        ObterDimensoesVideo(quadrosNoMundo[slotMundoDisponivel].caminhoOriginal,
                                            &quadrosNoMundo[slotMundoDisponivel].videoLargura,
                                            &quadrosNoMundo[slotMundoDisponivel].videoAltura,
                                            &quadrosNoMundo[slotMundoDisponivel].temAudio);

                        Image imgEstavel = GenImageColor(quadrosNoMundo[slotMundoDisponivel].videoLargura,
                                                         quadrosNoMundo[slotMundoDisponivel].videoAltura, BLANK);
                        ImageFormat(&imgEstavel, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                        quadrosNoMundo[slotMundoDisponivel].textura = LoadTextureFromImage(imgEstavel);
                        UnloadImage(imgEstavel);

                        // Passa o slot disponível como ID único para o arquivo temporário de som
                        IniciarPipeVideoEAudio(&quadrosNoMundo[slotMundoDisponivel], slotMundoDisponivel);
                    } else {
                        quadrosNoMundo[slotMundoDisponivel].textura = inventario[slotSelecionado];
                        quadrosNoMundo[slotMundoDisponivel].temAudio = false;
                    }

                    quadrosNoMundo[slotMundoDisponivel].box = (BoundingBox){
                        (Vector3){ posicaoSpawn.x - larg/2, posicaoSpawn.y - alt/2, posicaoSpawn.z - esp/2 },
                        (Vector3){ posicaoSpawn.x + larg/2, posicaoSpawn.y + alt/2, posicaoSpawn.z + esp/2 }
                    };

                    quadrosNoMundo[slotMundoDisponivel].ativo = true;
                    inventario[slotSelecionado] = (Texture2D){ 0 };
                    inventarioEhAnimado[slotSelecionado] = false;
                    sprintf(statusTexto, "Mídia inserida em ALTA RESOLUÇÃO!");
                }
            }
        }

        // Deletar objeto do mundo (Tecla DEL)
        if (IsKeyPressed(KEY_DELETE) && quadroMiradoIndex != -1 && quadroFocadoIndex == -1 && quadroArrastandoIndex == -1) {
            int idxRemover = quadroMiradoIndex;
            quadroMiradoIndex = -1;
            if (quadrosNoMundo[idxRemover].ativo) {
                FecharPipeVideoEAudio(&quadrosNoMundo[idxRemover]);
                UnloadTexture(quadrosNoMundo[idxRemover].textura);
                quadrosNoMundo[idxRemover].ativo = false;
                sprintf(statusTexto, "Objeto e áudio removidos com sucesso.");
            }
        }

        // Entrar e sair do modo Inspeção (Clique Esquerdo / Q)
        if (quadroMiradoIndex != -1 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && quadroFocadoIndex == -1 && quadroArrastandoIndex == -1) {
            quadroFocadoIndex = quadroMiradoIndex;
            inspecaoZoom = 1.0f; inspecaoPan = (Vector2){ 0.0f, 0.0f };
            EnableCursor();
        }
        if (quadroFocadoIndex != -1 && IsKeyPressed(KEY_Q)) {
            quadroFocadoIndex = -1;
            DisableCursor();
        }

        // --- DESENHO ---
        BeginDrawing();
        ClearBackground(DARKGRAY);

        BeginMode3D(camera);
        DrawGrid(30, 1.0f);
        for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
            if (!quadrosNoMundo[i].ativo) continue;

            if (i == quadroArrastandoIndex) DrawBoundingBox(quadrosNoMundo[i].box, BLUE);
            else if (i == quadroMiradoIndex) {
                DrawBoundingBox(quadrosNoMundo[i].box, quadrosNoMundo[i].pausado ? ORANGE : YELLOW);
            }

            DrawBillboard(camera, quadrosNoMundo[i].textura, quadrosNoMundo[i].posicao, quadrosNoMundo[i].altura, WHITE);
        }
        EndMode3D();

        if (quadroFocadoIndex != -1) {
            Texture2D tex = quadrosNoMundo[quadroFocadoIndex].textura;
            float escalaBase = (tex.width > tex.height) ? ((float)screenWidth / tex.width * 0.5f) : ((float)screenHeight / tex.height * 0.7f);
            float escalaFinal = escalaBase * inspecaoZoom;
            Vector2 pos = { ((screenWidth/2.0f)-((tex.width*escalaFinal)/2.0f))+inspecaoPan.x, ((screenHeight/2.0f)-((tex.height*escalaFinal)/2.0f))+inspecaoPan.y };
            DrawTextureEx(tex, pos, 0.0f, escalaFinal, WHITE);

            DrawRectangle(0, 0, screenWidth, 45, Fade(BLACK, 0.7f));
            DrawText("Visualizando Mídia Ativa | Scroll: Zoom | Arraste Mouse: Pan | Q: Voltar", 20, 12, 16, LIGHTGRAY);
        } else {
            DrawCircle(screenWidth / 2, screenHeight / 2, 4, RED);
            DrawText("WASD: Andar | 1-9: Hotbar | E: Fixar | Segurar V: Arrastar | Clique Dir: Pausar | DEL: Deletar", 20, 20, 18, WHITE);

            if (quadroMiradoIndex != -1 && quadrosNoMundo[quadroMiradoIndex].ehAnimado && quadrosNoMundo[quadroMiradoIndex].pausado) {
                DrawText("MÍDIA PAUSADA (Clique Direito para Retomar)", 20, 50, 18, ORANGE);
            } else {
                DrawText(statusTexto, 20, 50, 18, GREEN);
            }

            // --- HOTBAR ---
            int itemEspacamento = 90;
            int itemLargura = 80;
            int hotbarLargura = (MAX_INVENTARIO * itemEspacamento) + 10;
            int hotbarX = (screenWidth / 2) - (hotbarLargura / 2);
            int hotbarY = screenHeight - 90;

            DrawRectangle(hotbarX, hotbarY - 10, hotbarLargura, 80, Fade(BLACK, 0.6f));

            for (int i = 0; i < MAX_INVENTARIO; i++) {
                int slotX = hotbarX + 10 + (i * itemEspacamento);

                DrawRectangleLines(slotX, hotbarY, itemLargura, 60, (i == slotSelecionado) ? GREEN : GRAY);

                if (inventario[i].id > 0) {
                    float escalaSlot = (inventario[i].width > inventario[i].height) ? (80.0f / inventario[i].width) : (60.0f / inventario[i].height);
                    Vector2 posSlot = { slotX + (80 - inventario[i].width*escalaSlot)/2, hotbarY + (60 - inventario[i].height*escalaSlot)/2 };
                    DrawTextureEx(inventario[i], posSlot, 0.0f, escalaSlot, WHITE);

                    if (inventarioEhAnimado[i]) {
                        DrawRectangle(slotX + 5, hotbarY + 45, 45, 12, RED);
                        DrawText("HQ-STREAM", slotX + 6, hotbarY + 47, 8, WHITE);
                    }
                } else {
                    DrawTexture(texSlotVazio, slotX, hotbarY, WHITE);
                }

                char numStr[2];
                sprintf(numStr, "%d", i + 1);
                DrawText(numStr, slotX + 4, hotbarY + 4, 10, (i == slotSelecionado) ? GREEN : LIGHTGRAY);
            }
        }
        EndDrawing();
    }

    // Limpeza da Memória
    for (int i = 0; i < MAX_MUNDO_QUALQUER; i++) {
        if (quadrosNoMundo[i].ativo) {
            if (quadrosNoMundo[i].ehAnimado) FecharPipeVideoEAudio(&quadrosNoMundo[i]);
            UnloadTexture(quadrosNoMundo[i].textura);
        }
    }
    for (int i = 0; i < MAX_INVENTARIO; i++) if (inventario[i].id > 0) UnloadTexture(inventario[i]);
    UnloadTexture(texSlotVazio);

    CloseAudioDevice();
    CloseWindow();
    return 0;
}
