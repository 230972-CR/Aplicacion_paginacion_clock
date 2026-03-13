/*
=======================================================================
  PAGINACION EN TIEMPO REAL - ALGORITMO RELOJ (CLOCK)
  Alumno  : Carlos | Ryzen 7 7445HS | 16 GB RAM | Windows 11

  QUE HACE:
  - Corre un proceso REAL (suma de arreglos de ~400 MB)
  - Reserva 4 GB adicionales con VirtualAlloc para llenar la RAM
  - El SO Windows hace la paginacion REAL en respuesta
  - Lee datos REALES del kernel (GlobalMemoryStatusEx, GetProcessMemoryInfo)
  - Muestra en paralelo el Algoritmo Reloj simulado con esos datos
  - Cubre TODOS los conceptos de la pizarra:
      MMU, TLB, bits P/R/D/V, dir logica/fisica, PTBR,
      espacio de direcciones, mapa de bits, lista de huecos libres,
      page faults reales, working set, page file

  COMPILE:
    g++ -O0 -o paginacion_real.exe paginacion_real.cpp -std=c++17
  RUN (como Administrador para mejores resultados):
    paginacion_real.exe

  SEGURO: al cerrar el programa, toda la RAM se libera al instante.
  Tu PC volvera a la normalidad inmediatamente.
=======================================================================
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <sstream>
#include <chrono>
#include <random>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <tuple>

#pragma comment(lib, "psapi.lib")

// ── Cuanta RAM reservar adicionalmente al proceso suma ───────────────
// Con 16 GB instalados, 4 GB es seguro y visible en el Admin de Tareas
// Cambia a 8 si quieres mas presion (PC ira mas lenta mientras corre)
static const size_t RAM_EXTRA_GB    = 1;
static const unsigned long long RAM_EXTRA_BYTES = (unsigned long long)RAM_EXTRA_GB * 1024ULL * 1024ULL * 1024ULL;
static const size_t BLOQUE          = 256ULL*1024*1024; // 256 MB por bloque

// ── El proceso real: arreglo de 100 millones de enteros (~800 MB) ────
static const int    N_ELEM          = 100*1024*1024;

// ── Simulacion Clock ─────────────────────────────────────────────────
static const int    SIM_FRAMES      = 6;   // marcos de pagina simulados
static const int    TLB_SIM         = 8;   // entradas TLB simuladas

// ── Constantes del sistema ───────────────────────────────────────────
static const size_t PAGE_BYTES      = 4096;
static const size_t GB              = 1024ULL*1024*1024;

// ════════════════════════════════════════════════════════════════════
//  COLORES (nombres sin conflicto con WinAPI)
// ════════════════════════════════════════════════════════════════════
static HANDLE hCon;
void C(WORD w){ SetConsoleTextAttribute(hCon, w); }
void R(){ C(7); }
static const WORD CYN=11,AMR=14,VRD=10,ROJ=12,BLC=15,GRI=8,MGA=13,AZL=9;

// ════════════════════════════════════════════════════════════════════
//  HELPERS
// ════════════════════════════════════════════════════════════════════
std::string fmt_gb(unsigned long long b){
    std::ostringstream s;
    s<<std::fixed<<std::setprecision(2)<<(double)b/GB<<" GB"; return s.str();
}
std::string fmt_mb(size_t b){
    std::ostringstream s;
    s<<std::fixed<<std::setprecision(1)<<(double)b/(1024.0*1024.0)<<" MB"; return s.str();
}
std::string fmt_pct(double v){
    std::ostringstream s; s<<std::fixed<<std::setprecision(1)<<v*100<<"%"; return s.str();
}
std::string barra(int p, int w=24){
    if(p<0)p=0; if(p>100)p=100;
    int f=p*w/100; std::string r="[";
    for(int i=0;i<w;i++) r+=(i<f?"#":"-");
    return r+"] "+std::to_string(p)+"%";
}
void linea(int n=68,char c='-'){ std::cout<<"  "; for(int i=0;i<n;i++) std::cout<<c; std::cout<<"\n"; }
void titulo(const char* t){
    C(AMR); std::cout<<"\n"; linea(68,'=');
    std::cout<<"  "<<t<<"\n"; linea(68,'='); R();
}
void sub(const char* t){ C(CYN); std::cout<<"  -- "<<t<<" --\n"; R(); }

// ════════════════════════════════════════════════════════════════════
//  ESTRUCTURAS DE DATOS
// ════════════════════════════════════════════════════════════════════

// Snapshot real del kernel en un instante
struct KernelMem {
    // Sistema global
    unsigned long long ram_total;
    unsigned long long ram_usada;
    unsigned long long ram_libre;
    DWORD              pct_ram;
    unsigned long long pf_total;       // page file total
    unsigned long long pf_usado;       // page file en uso (swap activo)
    unsigned long long vm_total;       // espacio virtual total del proceso
    unsigned long long vm_libre;
    // Este proceso
    SIZE_T ws_actual;                  // working set: RAM fisica usada por el proceso
    SIZE_T ws_peak;
    SIZE_T vm_privado;                 // memoria virtual privada comprometida
    DWORD  pf_acum;                    // page faults acumulados del proceso
    DWORD  pf_delta;                   // page faults desde el ultimo muestreo
    // Calculados
    unsigned long long marcos_libres;  // frames libres en el sistema
    unsigned long long marcos_proceso; // frames usados por este proceso
};

// Marco del Algoritmo Reloj simulado
struct Marco {
    int    pag_id = -1;
    bool   P      = false; // Presencia: pagina en RAM
    bool   R_bit  = false; // Referencia: usada recientemente (clave del Reloj)
    bool   D      = false; // Dirty/Sucio: modificada, debe ir a disco antes de reemplazar
    bool   V      = false; // Valido: entrada de tabla de paginas valida
    unsigned long long dir_logica = 0; // dir virtual generada por CPU
    unsigned long long dir_fisica = 0; // dir fisica en RAM (simulada)
    int    accesos = 0;
};

// Entrada TLB simulada (cache de la MMU)
struct EntradaTLB {
    int  pag_id = -1;
    int  marco  = -1;
    bool valida = false;
    int  hits   = 0;
};

// Metricas del algoritmo
struct Metricas {
    int refs=0, fallos=0, aciertos=0, reemplazos=0;
    int escr_disco=0, giros=0, seg_op=0;
    int tlb_hits=0, tlb_misses=0;
    double t_fallos=0, t_hits=0, t_tlb=0;
};

// ════════════════════════════════════════════════════════════════════
//  ESTADO GLOBAL (compartido entre hilos)
// ════════════════════════════════════════════════════════════════════
std::atomic<bool>      g_correr{true};
std::atomic<long long> g_suma{0};
std::atomic<long long> g_iter{0};
std::atomic<unsigned long long> g_reservado{0};
std::atomic<int>       g_pf_generados{0}; // page faults generados por hiloRAM

// Memoria del proceso real
std::vector<int> g_arr_A;
std::vector<int> g_arr_B;
std::vector<long long> g_arr_R; // resultado

// Bloques extra para llenar la RAM
std::vector<unsigned char*> g_bloques;

// ════════════════════════════════════════════════════════════════════
//  LEER DATOS REALES DEL KERNEL (Windows API)
// ════════════════════════════════════════════════════════════════════
static DWORD g_pf_ant = 0;

KernelMem leerKernel(){
    MEMORYSTATUSEX ms; ms.dwLength=sizeof(ms); GlobalMemoryStatusEx(&ms);
    PROCESS_MEMORY_COUNTERS pmc; GetProcessMemoryInfo(GetCurrentProcess(),&pmc,sizeof(pmc));
    KernelMem k;
    k.ram_total    = ms.ullTotalPhys;
    k.ram_libre    = ms.ullAvailPhys;
    k.ram_usada    = ms.ullTotalPhys - ms.ullAvailPhys;
    k.pct_ram      = ms.dwMemoryLoad;
    k.pf_total     = ms.ullTotalPageFile;
    k.pf_usado     = ms.ullTotalPageFile - ms.ullAvailPageFile;
    k.vm_total     = ms.ullTotalVirtual;
    k.vm_libre     = ms.ullAvailVirtual;
    k.ws_actual    = pmc.WorkingSetSize;
    k.ws_peak      = pmc.PeakWorkingSetSize;
    k.vm_privado   = pmc.PagefileUsage;
    k.pf_acum      = pmc.PageFaultCount;
    k.pf_delta     = pmc.PageFaultCount - g_pf_ant;
    g_pf_ant       = pmc.PageFaultCount;
    k.marcos_libres  = ms.ullAvailPhys / PAGE_BYTES;
    k.marcos_proceso = pmc.WorkingSetSize / PAGE_BYTES;
    return k;
}

// ════════════════════════════════════════════════════════════════════
//  HILO 1: PROCESO REAL - Suma de arreglos grandes
//  Accede a ~800 MB de datos de forma secuencial y aleatoria.
//  Genera page faults reales cuando la RAM esta llena.
// ════════════════════════════════════════════════════════════════════
DWORD WINAPI hiloProcesoSuma(LPVOID){
    std::mt19937 rng(99);
    long long suma = 0;
    long long iter = 0;
    while(g_correr){
        // Fase A: recorrido SECUENCIAL (localidad espacial -> TLB eficiente)
        for(int i=0; i<N_ELEM && g_correr; i++){
            g_arr_R[i] = (long long)g_arr_A[i] + g_arr_B[i] + iter;
            suma += g_arr_R[i];
        }
        // Fase B: recorrido ALEATORIO (rompe localidad -> mas page faults)
        for(int k=0; k<N_ELEM/8 && g_correr; k++){
            int i = rng() % N_ELEM;
            int j = rng() % N_ELEM;
            g_arr_R[i] += g_arr_B[j];
            suma += g_arr_R[i];
        }
        iter++;
        g_suma.store(suma % 1000000000LL); // evitar overflow display
        g_iter.store(iter);
        Sleep(15);
    }
    return 0;
}

// ════════════════════════════════════════════════════════════════════
//  HILO 2: LLENAR RAM con VirtualAlloc + memset
//  Reserva RAM_EXTRA_GB GB adicionales para presionar al SO.
//  Cada memset() toca cada pagina de 4 KB -> page fault real por pagina.
//  Mantiene accesos aleatorios para que el SO no libere las paginas.
// ════════════════════════════════════════════════════════════════════
DWORD WINAPI hiloLlenarRAM(LPVOID){
    std::mt19937_64 rng(1337);
    unsigned long long total = 0;

    while(g_correr && total < RAM_EXTRA_BYTES){
        unsigned long long este = (RAM_EXTRA_BYTES - total < BLOQUE) ? (RAM_EXTRA_BYTES - total) : BLOQUE;
        unsigned char* ptr = (unsigned char*)VirtualAlloc(
            NULL, este, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        if(!ptr) break;
        // Tocar cada pagina = page fault real del OS
        for(size_t off = 0; off < este; off += PAGE_BYTES){
            ptr[off] = (unsigned char)(total & 0xFF);
            g_pf_generados.fetch_add(1);
        }
        g_bloques.push_back(ptr);
        total += este;
        g_reservado.store(total);
        Sleep(5);
    }

    // Acceso aleatorio continuo: evita que Windows recupere las paginas
    size_t nb = g_bloques.size();
    while(g_correr && nb > 0){
        size_t b   = rng() % nb;
        size_t off = (rng() % (BLOQUE/sizeof(long long))) * sizeof(long long);
        long long* p = (long long*)(g_bloques[b] + off);
        *p += 1;
        if(g_iter.load() % 5000 == 0)
            Sleep(1);
    }

    for(auto p : g_bloques) VirtualFree(p, 0, MEM_RELEASE);
    g_bloques.clear();
    return 0;
}

// ════════════════════════════════════════════════════════════════════
//  ALGORITMO RELOJ (Clock / Segunda Oportunidad)
//  Se ejecuta sobre la SECUENCIA DEL ENUNCIADO en paralelo al
//  proceso real. Sus datos reales vienen del kernel en cada paso.
// ════════════════════════════════════════════════════════════════════
class Reloj {
public:
    std::vector<Marco>      frames;
    std::vector<EntradaTLB> tlb;
    int ptr = 0;
    Metricas m;

    Reloj(): frames(SIM_FRAMES), tlb(TLB_SIM){}

    // Retorna {fue_fallo, victima, log}
    std::tuple<bool,int,std::string> acceder(int pag, bool write){
        m.refs++;
        std::string log;

        // 1. TLB
        int ti = buscarTLB(pag);
        if(ti >= 0){ m.tlb_hits++;  log += "TLB HIT  | "; tlb[ti].hits++; }
        else        { m.tlb_misses++; log += "TLB MISS | "; }

        // 2. Hit en RAM
        for(int i=0;i<SIM_FRAMES;i++){
            if(frames[i].pag_id == pag){
                frames[i].R_bit = true;
                frames[i].accesos++;
                if(write) frames[i].D = true;
                m.aciertos++;
                updTLB(pag,i);
                log += "ACIERTO marco="+std::to_string(i);
                recalc(); return {false,-1,log};
            }
        }

        // 3. Fallo
        m.fallos++; m.reemplazos++;
        log += "FALLO -> ";

        // 3a. Marco libre
        for(int i=0;i<SIM_FRAMES;i++){
            if(frames[i].pag_id == -1){
                cargar(i,pag,write); updTLB(pag,i);
                log += "marco libre="+std::to_string(i);
                recalc(); return {true,-1,log};
            }
        }

        // 3b. Girar reloj
        log += "girar[";
        int victima = -1;
        while(true){
            Marco& f = frames[ptr];
            m.giros++;
            if(!f.R_bit){
                victima = f.pag_id;
                if(f.D){ m.escr_disco++; log += "pag"+std::to_string(victima)+"(D=1->disco)"; }
                else   {                 log += "pag"+std::to_string(victima)+"(D=0->desc)";  }
                invalidarTLB(victima);
                cargar(ptr,pag,write); updTLB(pag,ptr);
                ptr = (ptr+1)%SIM_FRAMES;
                break;
            } else {
                log += "pag"+std::to_string(f.pag_id)+"(R=0), ";
                f.R_bit = false;
                m.seg_op++;
                ptr = (ptr+1)%SIM_FRAMES;
            }
        }
        log += "]";
        recalc(); return {true,victima,log};
    }

    int buscarTLB(int p) const {
        for(int i=0;i<TLB_SIM;i++) if(tlb[i].valida&&tlb[i].pag_id==p) return i;
        return -1;
    }
    void updTLB(int p, int m_){
        int i=buscarTLB(p); if(i>=0){tlb[i].hits++;return;}
        for(auto& e:tlb) if(!e.valida){e={p,m_,true,1};return;}
        for(int i=0;i<TLB_SIM-1;i++) tlb[i]=tlb[i+1];
        tlb[TLB_SIM-1]={p,m_,true,1};
    }
    void invalidarTLB(int p){ for(auto& e:tlb) if(e.pag_id==p) e.valida=false; }

private:
    void cargar(int i, int p, bool w){
        auto& f=frames[i];
        f.pag_id=p; f.P=true; f.R_bit=true; f.D=w; f.V=true; f.accesos=1;
        f.dir_logica = (unsigned long long)p * PAGE_BYTES;
        f.dir_fisica = 0x100000ULL + (unsigned long long)i * PAGE_BYTES;
    }
    void recalc(){
        int t=m.refs;
        m.t_fallos = t?(double)m.fallos/t:0;
        m.t_hits   = t?(double)m.aciertos/t:0;
        m.t_tlb    = t?(double)m.tlb_hits/t:0;
    }
};

// ════════════════════════════════════════════════════════════════════
//  IMPRESION - BLOQUE 1: HARDWARE Y ESTADO REAL DEL SISTEMA
// ════════════════════════════════════════════════════════════════════
void imp_hardware(const KernelMem& k, int paso){
    titulo("[1] HARDWARE Y ESTADO REAL DEL SISTEMA (datos del kernel)");

    sub("CPU y MMU (tu hardware real)");
    C(BLC);
    std::cout<<"  Procesador   : AMD Ryzen 7 7445HS @ 3.20 GHz | 8 nucleos | x86-64\n";
    std::cout<<"  RAM instalada: 16.0 GB  |  Usable: 15.3 GB\n";
    std::cout<<"  Tamano pagina: 4,096 bytes (4 KB) -- estandar x64 Windows\n";
    std::cout<<"  MMU          : integrada en CPU, traduce dir.logica -> dir.fisica\n";
    std::cout<<"  TLB real     : L1=64 entradas (4KB pages) | L2=2048 entradas\n";
    std::cout<<"  Registro PTBR: apunta a tabla de paginas del proceso en RAM\n";
    std::cout<<"  Reg. limite  : define rango valido de dir. logicas del proceso\n";
    R();

    sub("RAM Fisica (lectura en tiempo real - GlobalMemoryStatusEx)");
    int pct = k.pct_ram;
    C(pct>90?ROJ:pct>70?AMR:VRD);
    std::cout<<"  En uso       : "<<fmt_gb(k.ram_usada)<<"  "<<barra(pct)<<"\n"; R();
    std::cout<<"  Disponible   : "<<fmt_gb(k.ram_libre)<<"\n";
    std::cout<<"  Total        : "<<fmt_gb(k.ram_total)<<"\n";
    C(VRD);
    std::cout<<"  Marcos libres: "<<k.marcos_libres<<" frames de 4 KB disponibles ahora\n"; R();
    std::cout<<"  Marcos totales del sistema: "<<(k.ram_total/PAGE_BYTES)<<"\n";

    sub("Espacio de Direcciones (pizarra: registro base + limite)");
    C(BLC);
    std::cout<<"  Dir. logica  : generada por la CPU en cada instruccion MOV/LOAD/STORE\n";
    std::cout<<"                 Ejemplo: MOV EAX,[0x7FF000] -> dir logica = 0x7FF000\n";
    std::cout<<"  Traduccion   : Dir_logica --[MMU+TLB]--> Dir_fisica en RAM\n";
    std::cout<<"  Espacio/proc : "<<fmt_gb(k.vm_total)<<"  (Windows 11 x64)\n";
    std::cout<<"  VM disponible: "<<fmt_gb(k.vm_libre)<<"\n"; R();

    sub("Page File (swap en disco - lista de huecos)");
    int pfp = k.pf_total?(int)((k.pf_usado*100)/k.pf_total):0;
    C(pfp>60?ROJ:pfp>30?AMR:VRD);
    std::cout<<"  En uso       : "<<fmt_gb(k.pf_usado)<<" / "<<fmt_gb(k.pf_total)
             <<"  "<<barra(pfp)<<"\n"; R();
    C(GRI);
    if(pfp > 10)
        std::cout<<"  ** El SO esta paginando a disco -- exactamente lo que el Reloj evita **\n";
    else
        std::cout<<"  El SO aun no necesita swap masivo\n";
    R();

    sub("Este proceso (paginacion_real.exe) - GetProcessMemoryInfo");
    C(CYN);
    std::cout<<"  Working Set  : "<<fmt_mb(k.ws_actual)
             <<"  <- RAM fisica real usada ahora\n";
    std::cout<<"  Peak WS      : "<<fmt_mb(k.ws_peak)<<"\n";
    std::cout<<"  Mem.virtual  : "<<fmt_mb(k.vm_privado)<<"  <- comprometida\n";
    std::cout<<"  Marcos/proc  : "<<k.marcos_proceso<<" frames de 4KB en uso\n";
    C(ROJ);
    std::cout<<"  Page Faults  : "<<k.pf_acum<<" acumulados";
    if(k.pf_delta > 0){
        C(AMR);
        std::cout<<"  (+"<<k.pf_delta<<" nuevos desde el paso anterior)";
    }
    std::cout<<"\n"; R();
    C(GRI);
    std::cout<<"  [Admin Tareas > Rendimiento > Memoria para ver esto en vivo]\n"; R();

    sub("Presion de memoria activa (hilo VirtualAlloc)");
    size_t res = g_reservado.load();
    int rp = (RAM_EXTRA_BYTES>0)?(int)((res*100)/RAM_EXTRA_BYTES):0;
    C(rp>=100?ROJ:rp>50?AMR:MGA);
    std::cout<<"  Reservado    : "<<fmt_gb(res)<<" / "<<RAM_EXTRA_GB
             <<" GB  "<<barra(rp)<<"\n"; R();
    std::cout<<"  PF generados : "<<g_pf_generados.load()
             <<" (1 por cada pagina de 4KB tocada)\n";
    std::cout<<"  Suma proceso : "<<g_suma.load()
             <<"  |  Iteracion: "<<g_iter.load()<<"\n";
}

// ════════════════════════════════════════════════════════════════════
//  IMPRESION - BLOQUE 2: TABLA DE PAGINACION
// ════════════════════════════════════════════════════════════════════
void imp_tabla(const std::vector<Marco>& frames, int ptr_reloj){
    titulo("[2] TABLA DE PAGINACION DEL PROCESO (marcos simulados)");
    C(GRI);
    std::cout<<"  Cada fila = un marco de pagina fisico en RAM.\n";
    std::cout<<"  Bits: [P]=Presencia  [R]=Referencia(2da oport.)  [D]=Dirty/Sucio  [V]=Valido\n\n";
    R();
    C(AMR);
    std::cout<<"  Ptr  Marco  Pagina  [P]  [R]  [D]  [V]  Accesos  Dir.Logica    Dir.Fisica    Estado\n";
    linea(80,'-');
    R();

    for(int i=0;i<SIM_FRAMES;i++){
        const Marco& f = frames[i];
        if(i==ptr_reloj){ C(CYN); std::cout<<"  >>>"; }
        else             { C(GRI); std::cout<<"    "; }
        C(BLC); std::cout<<std::setw(3)<<i<<"   ";

        if(f.pag_id == -1){
            C(GRI);
            std::cout<<"  ---    -    -    -    -     ---      --------      --------      [LIBRE]\n";
        } else {
            C(BLC); std::cout<<std::setw(4)<<f.pag_id<<"    ";
            // P
            f.P?C(VRD):C(ROJ); std::cout<<(f.P?" 1 ":" 0 ")<<"  ";
            // R
            f.R_bit?C(VRD):C(AMR); std::cout<<(f.R_bit?" 1 ":" 0 ")<<"  ";
            // D
            f.D?C(ROJ):C(VRD); std::cout<<(f.D?" 1 ":" 0 ")<<"  ";
            // V
            f.V?C(VRD):C(ROJ); std::cout<<(f.V?" 1 ":" 0 ");
            // accesos
            C(BLC); std::cout<<std::setw(7)<<f.accesos<<"  ";
            // dirs
            C(GRI);
            std::cout<<"0x"<<std::hex<<std::setw(6)<<std::setfill('0')<<f.dir_logica<<"  ";
            std::cout<<"0x"<<std::setw(6)<<std::setfill('0')<<f.dir_fisica;
            std::cout<<std::dec<<std::setfill(' ');
            // estado
            if( f.R_bit &&  f.D){ C(AMR); std::cout<<"  [PROTEG+MOD]"; }
            else if( f.R_bit)   { C(VRD); std::cout<<"  [PROTEGIDA] "; }
            else if( f.D)       { C(ROJ); std::cout<<"  [SUCIA-CAND]"; }
            else                { C(MGA); std::cout<<"  [CANDIDATA] "; }
            std::cout<<"\n";
        }
        R();
    }
    C(CYN); std::cout<<"  >>> Puntero Reloj en marco "<<ptr_reloj<<"\n"; R();
    C(GRI);
    std::cout<<"  [PROTEGIDA]=R=1 no se reemplaza  |  [CANDIDATA]=R=0 proxima victima\n";
    std::cout<<"  [SUCIA-CAND]=R=0 y D=1, debe escribirse a disco antes de reemplazar\n";
    R();
}

// ════════════════════════════════════════════════════════════════════
//  IMPRESION - BLOQUE 3: TLB
// ════════════════════════════════════════════════════════════════════
void imp_tlb(const std::vector<EntradaTLB>& tlb, int pag_actual){
    titulo("[3] TLB - Translation Lookaside Buffer (Cache de la MMU)");
    C(GRI);
    std::cout<<"  La TLB almacena traducciones recientes: pagina -> marco fisico\n";
    std::cout<<"  HIT : pagina encontrada en TLB -> acceso en ~1 ciclo de CPU\n";
    std::cout<<"  MISS: no esta en TLB -> consultar tabla en RAM (~100 ciclos)\n\n";
    R();
    C(AMR);
    std::cout<<"  Slot  Pagina  Marco  Hits  Estado\n";
    linea(40,'-');
    R();
    for(int i=0;i<TLB_SIM;i++){
        const EntradaTLB& e = tlb[i];
        if(e.valida){
            bool actual = (e.pag_id == pag_actual);
            actual ? C(VRD) : C(BLC);
            std::cout<<"   "<<std::setw(2)<<i<<"    "<<std::setw(3)<<e.pag_id
                     <<"     "<<std::setw(2)<<e.marco<<"    "<<std::setw(3)<<e.hits<<"   "
                     <<(actual?"<< PAGINA ACCEDIDA AHORA":"en cache")<<"\n";
        } else {
            C(GRI);
            std::cout<<"   "<<std::setw(2)<<i<<"     --      --     --    [vacia]\n";
        }
        R();
    }
}

// ════════════════════════════════════════════════════════════════════
//  IMPRESION - BLOQUE 4: ALGORITMO RELOJ PASO A PASO
// ════════════════════════════════════════════════════════════════════
void imp_reloj(const std::vector<int>& seq,
               const std::vector<bool>& fallos,
               const std::vector<int>& reempl,
               const std::vector<std::string>& logs,
               int paso){
    titulo("[4] ALGORITMO RELOJ (Clock / Segunda Oportunidad) - Paso a paso");
    C(GRI);
    std::cout<<"  REGLA: recorrer marcos en circulo.\n";
    std::cout<<"         Si R=1 -> segunda oportunidad: poner R=0, avanzar.\n";
    std::cout<<"         Si R=0 -> esta es la victima: reemplazar.\n\n";
    R();
    C(AMR);
    std::cout<<"  Paso  Pag  Tipo      Victima  Log del algoritmo\n";
    linea(68,'-');
    R();
    for(int i=0;i<=paso&&i<(int)seq.size();i++){
        std::cout<<"  "<<std::setw(3)<<(i+1)<<"   "<<std::setw(3)<<seq[i]<<"  ";
        if(fallos[i]){
            C(ROJ); std::cout<<"FALLO     ";
            if(reempl[i]>=0){ C(AMR); std::cout<<"pag "<<std::setw(2)<<reempl[i]<<"   "; }
            else             { C(GRI); std::cout<<"  ---     "; }
        } else {
            C(VRD); std::cout<<"ACIERTO   ";
            C(GRI); std::cout<<"  ---     ";
        }
        R();
        C(GRI);
        if(i<(int)logs.size()) std::cout<<logs[i];
        std::cout<<"\n"; R();
    }
}

// ════════════════════════════════════════════════════════════════════
//  IMPRESION - BLOQUE 5: METRICAS
// ════════════════════════════════════════════════════════════════════
void imp_metricas(const Metricas& m, const KernelMem& k){
    titulo("[5] METRICAS COMPLETAS");

    auto row = [](const char* lab, std::string val, WORD col=BLC){
        C(GRI); std::cout<<"  | "; C(BLC);
        std::cout<<std::left<<std::setw(34)<<lab;
        C(GRI); std::cout<<"| "; C(col);
        std::cout<<std::left<<std::setw(20)<<val;
        C(GRI); std::cout<<"|\n"; R();
    };

    std::cout<<"\n  +------------------------------------+----------------------+\n";
    row("Referencias totales",             std::to_string(m.refs));
    row("Fallos de pagina  (Clock sim)",   std::to_string(m.fallos), ROJ);
    row("Aciertos en RAM   (Clock sim)",   std::to_string(m.aciertos), VRD);
    row("Reemplazos",                      std::to_string(m.reemplazos), AMR);
    row("Escrituras a disco (D=1 al sal)", std::to_string(m.escr_disco), ROJ);
    row("Giros del puntero Reloj",         std::to_string(m.giros), CYN);
    row("2das oportunidades dadas (R=0)",  std::to_string(m.seg_op), MGA);
    row("TLB Hits (sim)",                  std::to_string(m.tlb_hits), VRD);
    row("TLB Misses (sim)",                std::to_string(m.tlb_misses), AMR);
    row("Tasa de fallos",                  fmt_pct(m.t_fallos), ROJ);
    row("Tasa de aciertos",                fmt_pct(m.t_hits), VRD);
    row("Eficiencia TLB simulada",         fmt_pct(m.t_tlb), CYN);
    row("Page Faults REALES del OS",       std::to_string(k.pf_acum), MGA);
    row("PF generados por hiloRAM",        std::to_string(g_pf_generados.load()), AMR);
    row("Working Set real del proceso",    fmt_mb(k.ws_actual), CYN);
    std::cout<<"  +------------------------------------+----------------------+\n";

    std::cout<<"\n";
    if(m.t_hits>=0.7)     { C(VRD); std::cout<<"  ★ Buen rendimiento. El Reloj protege bien las paginas activas.\n"; }
    else if(m.t_hits>=0.4){ C(AMR); std::cout<<"  ◆ Rendimiento medio. Aumentar marcos (SIM_FRAMES) lo mejoraria.\n"; }
    else                  { C(ROJ); std::cout<<"  ✗ Thrashing. Demasiados fallos -- necesitas mas marcos.\n"; }
    R();
}

// ════════════════════════════════════════════════════════════════════
//  IMPRESION - BLOQUE 6: MAPA DE BITS + LISTA DE HUECOS LIBRES
// ════════════════════════════════════════════════════════════════════
void imp_mapa(const std::vector<Marco>& frames, const Reloj& rel){
    titulo("[6] MAPA DE BITS + LISTA DE HUECOS LIBRES");
    C(GRI);
    std::cout<<"  Mapa de bits: vista rapida de marcos ocupados (1) / libres (0)\n";
    std::cout<<"  Lista huecos: alternativa usada por algunos SO (Windows usa combinacion)\n\n";
    R();

    sub("Mapa de bits");
    std::cout<<"  Marco : ";
    for(int i=0;i<SIM_FRAMES;i++) std::cout<<std::setw(7)<<i;
    std::cout<<"\n  Bit P : ";
    for(auto& f:frames){ f.pag_id!=-1?C(VRD):C(GRI); std::cout<<std::setw(7)<<(f.pag_id!=-1?"1":"0"); }
    R(); std::cout<<"\n  Pagina: ";
    for(auto& f:frames){ C(BLC); std::cout<<std::setw(7)<<(f.pag_id!=-1?std::to_string(f.pag_id):"---"); }
    R(); std::cout<<"\n  Bit R : ";
    for(auto& f:frames){
        if(f.pag_id!=-1){ f.R_bit?C(VRD):C(ROJ); std::cout<<std::setw(7)<<(f.R_bit?"1":"0"); }
        else{ C(GRI); std::cout<<std::setw(7)<<"---"; }
    }
    R(); std::cout<<"\n  Bit D : ";
    for(auto& f:frames){
        if(f.pag_id!=-1){ f.D?C(ROJ):C(VRD); std::cout<<std::setw(7)<<(f.D?"1":"0"); }
        else{ C(GRI); std::cout<<std::setw(7)<<"---"; }
    }
    R(); std::cout<<"\n  TLB   : ";
    for(auto& f:frames){
        bool hit = f.pag_id!=-1 && rel.buscarTLB(f.pag_id)>=0;
        hit?C(CYN):C(GRI); std::cout<<std::setw(7)<<(hit?"HIT":"---");
    }
    R(); std::cout<<"\n  Reloj : ";
    for(int i=0;i<SIM_FRAMES;i++){
        i==rel.ptr?C(CYN):C(GRI); std::cout<<std::setw(7)<<(i==rel.ptr?">>":"");
    }
    R(); std::cout<<"\n\n";

    sub("Lista de huecos libres");
    bool alguno = false;
    for(int i=0;i<SIM_FRAMES;i++){
        if(frames[i].pag_id == -1){
            C(VRD); std::cout<<"  Marco "<<i<<" -> LIBRE (disponible para proxima carga)\n";
            alguno = true; R();
        }
    }
    if(!alguno){
        C(ROJ); std::cout<<"  Sin marcos libres -- el Reloj debe girar para encontrar victima\n"; R();
    }
}


// ════════════════════════════════════════════════════════════════════
//  RESUMEN FINAL COMPLETO
// ════════════════════════════════════════════════════════════════════
void imp_resumen_final(
    const std::vector<int>& seq,
    const std::vector<bool>& fallos,
    const std::vector<int>& reempl,
    const std::vector<std::string>& logs,
    const Metricas& m,
    const KernelMem& k_inicio,
    const KernelMem& k_fin)
{
    titulo("RESUMEN FINAL - ALGORITMO RELOJ COMPLETADO");

    // ── Descripcion del proceso real ──────────────────────────────
    sub("El proceso que se ejecuto");
    C(BLC);
    std::cout<<"  Proceso     : Suma acumulativa de 2 arreglos de 100 millones de enteros\n";
    std::cout<<"  Memoria proc: ~800 MB (3 arreglos int/long de 100M elementos)\n";
    std::cout<<"  Presion RAM : "<<RAM_EXTRA_GB<<" GB adicionales via VirtualAlloc\n";
    std::cout<<"  Patron acc. : Fase A secuencial (buena localidad) +\n";
    std::cout<<"                Fase B aleatoria  (rompe localidad, genera PF reales)\n";
    std::cout<<"  Resultado   : "<<g_suma.load()<<" (suma acumulada)  |  "
             <<g_iter.load()<<" iteraciones completadas\n";
    R();

    // ── Tabla completa de todos los pasos ─────────────────────────
    sub("Tabla completa de accesos - todos los pasos");
    C(AMR);
    std::cout<<"  Paso  Pag  Tipo       Victima  Descripcion\n";
    linea(68,'-');
    R();
    int total_fallos=0, total_aciertos=0, total_escr=0;
    for(int i=0;i<(int)seq.size();i++){
        std::cout<<"  "<<std::setw(3)<<(i+1)<<"   "<<std::setw(3)<<seq[i]<<"  ";
        if(fallos[i]){
            C(ROJ); std::cout<<"FALLO      ";
            total_fallos++;
            if(reempl[i]>=0){
                C(AMR); std::cout<<"pag "<<std::setw(2)<<reempl[i]<<"   ";
            } else {
                C(GRI); std::cout<<"  ---     ";
            }
        } else {
            C(VRD); std::cout<<"ACIERTO    ";
            total_aciertos++;
            C(GRI); std::cout<<"  ---     ";
        }
        R(); C(GRI);
        if(i<(int)logs.size()) std::cout<<logs[i];
        std::cout<<"\n"; R();
    }

    // ── Metricas finales del algoritmo ────────────────────────────
    sub("Metricas del Algoritmo Reloj");
    C(BLC);
    std::cout<<"  Total referencias    : "<<m.refs<<"\n";
    C(ROJ);
    std::cout<<"  Fallos de pagina     : "<<m.fallos
             <<"  ("<<std::fixed<<std::setprecision(1)<<m.t_fallos*100<<"%)\n";
    C(VRD);
    std::cout<<"  Aciertos en RAM      : "<<m.aciertos
             <<"  ("<<std::fixed<<std::setprecision(1)<<m.t_hits*100<<"%)\n";
    C(AMR);
    std::cout<<"  Reemplazos           : "<<m.reemplazos<<"\n";
    C(ROJ);
    std::cout<<"  Escrituras a disco   : "<<m.escr_disco
             <<"  (paginas con D=1 al ser reemplazadas)\n";
    C(CYN);
    std::cout<<"  Giros del reloj      : "<<m.giros<<"\n";
    C(MGA);
    std::cout<<"  2das oportunidades   : "<<m.seg_op
             <<"  (paginas con R=1 perdonadas)\n";
    C(VRD);
    std::cout<<"  TLB Hits sim.        : "<<m.tlb_hits
             <<"  ("<<std::fixed<<std::setprecision(1)<<m.t_tlb*100<<"%)\n";
    C(AMR);
    std::cout<<"  TLB Misses sim.      : "<<m.tlb_misses<<"\n";
    R();

    // ── Comparacion RAM inicio vs fin ─────────────────────────────
    sub("Impacto real en la RAM del sistema (kernel)");
    C(BLC);
    std::cout<<"  RAM usada al inicio  : "<<fmt_gb(k_inicio.ram_usada)
             <<"  ("<<k_inicio.pct_ram<<"%)\n";
    std::cout<<"  RAM usada al final   : "<<fmt_gb(k_fin.ram_usada)
             <<"  ("<<k_fin.pct_ram<<"%)\n";
    long long delta_ram = (long long)k_fin.ram_usada - (long long)k_inicio.ram_usada;
    C(delta_ram>0?ROJ:VRD);
    std::cout<<"  Delta RAM            : "<<(delta_ram>0?"+":"")
             <<fmt_gb((unsigned long long)std::abs(delta_ram))<<"\n";
    C(BLC);
    std::cout<<"  Page Faults reales   : "<<k_fin.pf_acum<<" acumulados\n";
    std::cout<<"  Working Set final    : "<<fmt_mb(k_fin.ws_actual)<<"\n";
    std::cout<<"  Page File en uso     : "<<fmt_gb(k_fin.pf_usado)
             <<" / "<<fmt_gb(k_fin.pf_total)<<"\n";
    R();

    // ── Conceptos de la pizarra aplicados ─────────────────────────
    sub("Conceptos de la pizarra aplicados en esta ejecucion");
    C(CYN);  std::cout<<"  MMU         : "; R();
    std::cout<<"Traduce cada dir. logica del proceso a dir. fisica en RAM\n";
    C(CYN);  std::cout<<"  PTBR        : "; R();
    std::cout<<"El registro base apunta a la tabla de paginas del proceso\n";
    C(CYN);  std::cout<<"  TLB         : "; R();
    std::cout<<m.tlb_hits<<" hits de "<<m.refs<<" referencias -- cache de traducciones de la MMU\n";
    C(CYN);  std::cout<<"  Bit P       : "; R();
    std::cout<<"Presencia -- indica si la pagina esta en RAM o en disco\n";
    C(CYN);  std::cout<<"  Bit R       : "; R();
    std::cout<<"Referencia -- clave del Reloj, da segunda oportunidad ("<<m.seg_op<<" veces usado)\n";
    C(CYN);  std::cout<<"  Bit D       : "; R();
    std::cout<<"Dirty/Sucio -- "<<m.escr_disco<<" paginas debieron escribirse a disco al reemplazarse\n";
    C(CYN);  std::cout<<"  Dir.logica  : "; R();
    std::cout<<"Generada por CPU, traducida por MMU+TLB a dir. fisica\n";
    C(CYN);  std::cout<<"  Mapa bits   : "; R();
    std::cout<<"Estructura del SO para ver frames libres/ocupados rapidamente\n";
    C(CYN);  std::cout<<"  Page File   : "; R();
    std::cout<<"Swap en disco -- usado cuando la RAM se llena ("<<fmt_gb(k_fin.pf_usado)<<" en uso)\n";
    C(CYN);  std::cout<<"  Algoritmo   : "; R();
    std::cout<<"Reloj (Clock) -- aproximacion eficiente de LRU con bit R\n";

    // ── Evaluacion final ──────────────────────────────────────────
    std::cout<<"\n";
    linea(68,'=');
    if(m.t_hits>=0.6){
        C(VRD); std::cout<<"  RESULTADO: Buen rendimiento. "<<m.aciertos<<" aciertos de "<<m.refs
                         <<" referencias.\n";
    } else if(m.t_hits>=0.3){
        C(AMR); std::cout<<"  RESULTADO: Rendimiento medio. Con mas marcos habria menos fallos.\n";
    } else {
        C(ROJ); std::cout<<"  RESULTADO: Alto numero de fallos. La secuencia tiene poca localidad.\n";
    }
    R(); linea(68,'=');
}

// ════════════════════════════════════════════════════════════════════
//  MAIN
// ════════════════════════════════════════════════════════════════════
int main(){
    hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    ShowWindow(GetConsoleWindow(), SW_MAXIMIZE);

    // Privilegio para VirtualAlloc grande
    HANDLE hToken;
    if(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY, &hToken)){
        TOKEN_PRIVILEGES tp; tp.PrivilegeCount=1;
        LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, 0);
        CloseHandle(hToken);
    }

    // ── Pantalla de bienvenida ───────────────────────────────────────
    system("cls");
    C(CYN);
    std::cout<<"\n  ================================================================\n";
    std::cout<<"   PAGINACION EN TIEMPO REAL | Carlos | Ryzen 7 7445HS | 16GB\n";
    std::cout<<"   Algoritmo Reloj (Clock / Segunda Oportunidad)\n";
    std::cout<<"  ================================================================\n"; R();

    C(AMR);
    std::cout<<"\n  QUE VA A PASAR:\n"; R();
    std::cout<<"  1. Se asigna el proceso real (arreglos de ~800 MB para sumar)\n";
    std::cout<<"  2. Un hilo reserva "<<RAM_EXTRA_GB<<" GB adicionales de RAM\n";
    std::cout<<"     -> Cada pagina de 4 KB tocada = 1 page fault real del OS\n";
    std::cout<<"  3. El Algoritmo Reloj corre en paralelo con la secuencia del enunciado\n";
    std::cout<<"  4. En cada paso presionas ENTER y ves TODOS los datos reales\n\n";

    C(VRD);
    std::cout<<"  COMO VER EN EL ADMINISTRADOR DE TAREAS (abrelo AHORA):\n"; R();
    std::cout<<"  - Ctrl+Shift+Esc -> Rendimiento -> Memoria\n";
    std::cout<<"  Veras la RAM subir unos "<<RAM_EXTRA_GB<<" GB mas sobre el estado actual\n";

    // No podemos leer k aun, mensaje generico
    std::cout<<"  - Detalles -> paginacion_real.exe -> columna 'Errores de pagina'\n";
    std::cout<<"    (clic derecho en cabecera de columna para activarla)\n\n";

    C(ROJ);
    std::cout<<"  SEGURO: toda la RAM se libera al instante cuando cierres el programa.\n"; R();
    std::cout<<"\n  Iniciando en 3 segundos...\n";
    Sleep(3000);

    // ── Inicializar arreglos del proceso real ────────────────────────
    system("cls");
    C(AMR); std::cout<<"\n  Inicializando arreglos (~800 MB)...\n"; R();
    g_arr_A.resize(N_ELEM); g_arr_B.resize(N_ELEM); g_arr_R.resize(N_ELEM,0);
    for(int i=0;i<N_ELEM;i++){ g_arr_A[i]=i%1000; g_arr_B[i]=(i*3)%997; }
    std::cout<<"  Arreglos listos ("<<fmt_mb(sizeof(int)*N_ELEM*2+sizeof(long long)*N_ELEM)<<")\n";
    std::cout<<"  Iniciando hilos...\n";

    // ── Iniciar hilos ────────────────────────────────────────────────
    HANDLE hSuma = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)hiloProcesoSuma,NULL,0,NULL);
    HANDLE hRAM  = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)hiloLlenarRAM,NULL,0,NULL);
    Sleep(500);

    // ── Secuencia del enunciado: 1(1),2(2),...,21(5) ─────────────────
    // Los numeros son IDs de pagina; el numero entre parentesis es
    // el numero de orden del alumno en la secuencia.
    std::vector<int> secuencia = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 1,
        11,12,13,14,15, 7,16, 8,17, 1,
        18, 2,19, 3,20, 4,21, 5
    };
    int N = (int)secuencia.size();
    std::vector<bool>        fallos(N, false);
    std::vector<int>         reempl(N, -1);
    std::vector<std::string> logs(N);

    Reloj reloj;
    KernelMem k_inicio = leerKernel();

    // ── Bucle principal: un paso del Reloj por ENTER ─────────────────
    for(int paso=0; paso<N; paso++){
        // Alternar lectura/escritura para demostrar bit D (Dirty)
        bool write = (paso % 5 == 0);
        std::tuple<bool,int,std::string> res = reloj.acceder(secuencia[paso], write);
        bool fallo   = std::get<0>(res);
        int  victima = std::get<1>(res);
        std::string log_paso = std::get<2>(res);
        fallos[paso] = fallo;
        reempl[paso] = victima;
        logs[paso]   = log_paso;

        // Leer estado REAL del kernel en este momento
        KernelMem k = leerKernel();

        // Renderizar todo
        system("cls");
        C(CYN);
        std::cout<<"\n  [ Paso "<<std::setw(2)<<(paso+1)<<"/"<<N
                 <<"  |  Pagina: "<<secuencia[paso]
                 <<"  |  "<<(write?"ESCRITURA [D=1]":"LECTURA        ")
                 <<"  |  "<<(fallo?"*** FALLO DE PAGINA ***":"acierto en RAM ")
                 <<"  |  RAM: "<<k.pct_ram<<"% ]\n";
        R();

        imp_hardware(k, paso);
        imp_tabla(reloj.frames, reloj.ptr);
        imp_tlb(reloj.tlb, secuencia[paso]);
        imp_reloj(secuencia, fallos, reempl, logs, paso);
        imp_metricas(reloj.m, k);
        imp_mapa(reloj.frames, reloj);

        C(GRI);
        std::cout<<"\n  [ Avanzando automaticamente en 3 segundos... ]\n"; R();
        Sleep(3000);
    }

    // ── Reporte final ────────────────────────────────────────────────
    g_correr = false;
    WaitForSingleObject(hSuma, INFINITE); CloseHandle(hSuma);
    WaitForSingleObject(hRAM,  INFINITE); CloseHandle(hRAM);

    system("cls");
    KernelMem k_fin = leerKernel();
    imp_resumen_final(secuencia, fallos, reempl, logs, reloj.m, k_inicio, k_fin);
    imp_mapa(reloj.frames, reloj);
    C(VRD); std::cout<<"\n  Memoria liberada correctamente.\n";
    C(CYN); std::cout<<"  Programa terminado. Cerrando en 10 segundos...\n"; R();
    Sleep(10000);
    return 0;
}
