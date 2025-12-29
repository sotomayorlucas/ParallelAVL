#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════════════════════╗
║  COMPILER BATTLE: "JUICIO FINAL"                                             ║
║  Reproduce GCC vs ICX Hash Flooding Defense Breakage on Hybrid CPUs          ║
╚══════════════════════════════════════════════════════════════════════════════╝

Este script compara cómo diferentes compiladores/flags afectan la defensa 
contra Hash Flooding (AC-DoS) en procesadores híbridos (Intel Core Ultra).

Hallazgo: GCC -O3 puede romper la defensa (0% balance bajo ataque) debido a
optimizaciones agresivas como Loop Invariant Code Motion (LICM) en E-cores.

Uso:
    python run_compiler_battle.py
    python run_compiler_battle.py --compilers gcc,icpx
    python run_compiler_battle.py --runs 5
"""

import subprocess
import sys
import re
import os
import shutil
import argparse
from dataclasses import dataclass
from typing import Optional, List, Tuple
from pathlib import Path
import time


@dataclass
class CompilerConfig:
    """Configuración de un compilador para el benchmark."""
    name: str
    executable: str
    flags: List[str]
    description: str


@dataclass
class BenchResult:
    """Resultado de una ejecución del benchmark."""
    compiler: str
    balance_score: float
    hotspot_detected: bool
    suspicious_patterns: int
    blocked_redirects: int
    execution_time_ms: int
    raw_output: str
    success: bool
    error: Optional[str] = None


# Configuraciones de compiladores a probar
COMPILER_CONFIGS = {
    # GCC con diferentes niveles de optimización
    "gcc-O3": CompilerConfig(
        name="GCC -O3",
        executable="g++",
        flags=["-std=c++17", "-O3", "-Wall", "-Wextra", "-pthread"],
        description="GCC con optimización agresiva (puede romper defensa en CPUs híbridas)"
    ),
    "gcc-O2": CompilerConfig(
        name="GCC -O2", 
        executable="g++",
        flags=["-std=c++17", "-O2", "-Wall", "-Wextra", "-pthread"],
        description="GCC con optimización moderada"
    ),
    "gcc-O3-no-licm": CompilerConfig(
        name="GCC -O3 (no LICM)",
        executable="g++",
        flags=["-std=c++17", "-O3", "-fno-tree-loop-im", "-Wall", "-Wextra", "-pthread"],
        description="GCC O3 deshabilitando Loop Invariant Code Motion"
    ),
    "gcc-O3-barrier": CompilerConfig(
        name="GCC -O3 + Barrier Fix",
        executable="g++",
        flags=["-std=c++17", "-O3", "-DUSE_COMPILER_BARRIER", "-Wall", "-Wextra", "-pthread"],
        description="GCC O3 con compiler barrier habilitado"
    ),
    # Intel compilers (si están disponibles)
    "icpx": CompilerConfig(
        name="Intel ICX",
        executable="icpx",
        flags=["-std=c++17", "-O3", "-Wall", "-pthread"],
        description="Intel oneAPI DPC++/C++ Compiler"
    ),
    "icx": CompilerConfig(
        name="Intel ICX (legacy)",
        executable="icx",
        flags=["-std=c++17", "-O3", "-Wall", "-pthread"],
        description="Intel C++ Compiler (legacy name)"
    ),
    # Clang para referencia
    "clang": CompilerConfig(
        name="Clang -O3",
        executable="clang++",
        flags=["-std=c++17", "-O3", "-Wall", "-Wextra", "-pthread"],
        description="LLVM Clang para comparación"
    ),
}


def find_available_compilers() -> List[str]:
    """Detecta qué compiladores están disponibles en el sistema."""
    available = []
    for key, config in COMPILER_CONFIGS.items():
        if shutil.which(config.executable):
            available.append(key)
    return available


def compile_benchmark(config: CompilerConfig, source: Path, output: Path, 
                      include_dir: Path) -> Tuple[bool, str]:
    """Compila el benchmark con la configuración especificada."""
    cmd = [
        config.executable,
        *config.flags,
        f"-I{include_dir}",
        str(source),
        "-o", str(output)
    ]
    
    try:
        result = subprocess.run(
            cmd, 
            capture_output=True, 
            text=True, 
            timeout=120
        )
        if result.returncode != 0:
            return False, f"Compilation failed:\n{result.stderr}"
        return True, "OK"
    except subprocess.TimeoutExpired:
        return False, "Compilation timeout"
    except FileNotFoundError:
        return False, f"Compiler not found: {config.executable}"
    except Exception as e:
        return False, str(e)


def run_benchmark(binary: Path, timeout: int = 60) -> BenchResult:
    """Ejecuta el benchmark y parsea los resultados."""
    try:
        start = time.perf_counter()
        result = subprocess.run(
            [str(binary)],
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=binary.parent
        )
        elapsed = int((time.perf_counter() - start) * 1000)
        
        output = result.stdout + result.stderr
        
        # Parsear Balance Score (buscamos el mínimo bajo ataque)
        balance_scores = re.findall(r"Balance:\s+([\d.]+)%", output)
        min_balance = min([float(b) for b in balance_scores]) if balance_scores else 0.0
        
        # Parsear hotspot detection
        hotspot_matches = re.findall(r"Hotspot:\s+(YES|No)", output)
        has_hotspot = "YES" in hotspot_matches
        
        # Parsear suspicious patterns (sumamos todos)
        suspicious = re.findall(r"Suspicious:\s+(\d+)", output)
        total_suspicious = sum([int(s) for s in suspicious])
        
        # Parsear blocked redirects
        blocked = re.findall(r"Blocked:\s+(\d+)", output)
        total_blocked = sum([int(b) for b in blocked])
        
        return BenchResult(
            compiler="",
            balance_score=min_balance,
            hotspot_detected=has_hotspot,
            suspicious_patterns=total_suspicious,
            blocked_redirects=total_blocked,
            execution_time_ms=elapsed,
            raw_output=output,
            success=True
        )
        
    except subprocess.TimeoutExpired:
        return BenchResult(
            compiler="", balance_score=0, hotspot_detected=False,
            suspicious_patterns=0, blocked_redirects=0,
            execution_time_ms=timeout*1000, raw_output="",
            success=False, error="Benchmark timeout"
        )
    except Exception as e:
        return BenchResult(
            compiler="", balance_score=0, hotspot_detected=False,
            suspicious_patterns=0, blocked_redirects=0,
            execution_time_ms=0, raw_output="",
            success=False, error=str(e)
        )


def evaluate_result(result: BenchResult) -> str:
    """Evalúa si el resultado indica PASS o FAIL."""
    if not result.success:
        return "ERROR"
    
    # Criterios de éxito:
    # - Balance score > 50% bajo ataque indica defensa funcionando
    # - Balance score < 20% indica defensa rota
    if result.balance_score >= 50:
        return "PASS"
    elif result.balance_score >= 20:
        return "WARN"
    else:
        return "FAIL"


def print_banner():
    """Imprime el banner del script."""
    print("""
╔══════════════════════════════════════════════════════════════════════════════╗
║                                                                              ║
║   ⚔️  COMPILER BATTLE: JUICIO FINAL  ⚔️                                       ║
║                                                                              ║
║   Testing Hash Flooding Defense Under Different Compiler Optimizations       ║
║   Target: Hybrid CPU (Intel Core Ultra) LICM Optimization Bug                ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
""")


def print_result_table(results: List[BenchResult]):
    """Imprime tabla de resultados estilo reporte."""
    print("\n" + "═" * 80)
    print("  RESULTADOS DEL JUICIO FINAL")
    print("═" * 80)
    
    # Header
    print(f"{'Compiler':<25} {'Balance':<10} {'Status':<8} {'Hotspot':<10} {'Suspicious':<12} {'Time':<10}")
    print("-" * 80)
    
    for r in results:
        status = evaluate_result(r)
        status_icon = {"PASS": "✅", "WARN": "⚠️ ", "FAIL": "❌", "ERROR": "💥"}.get(status, "?")
        
        if r.success:
            print(f"{r.compiler:<25} {r.balance_score:>6.1f}%   {status_icon} {status:<5} "
                  f"{'YES' if r.hotspot_detected else 'No':<10} "
                  f"{r.suspicious_patterns:<12} {r.execution_time_ms}ms")
        else:
            print(f"{r.compiler:<25} {'N/A':<10} {status_icon} {status:<5} "
                  f"{'N/A':<10} {'N/A':<12} {r.error or 'Unknown error'}")
    
    print("═" * 80)


def print_summary(results: List[BenchResult]):
    """Imprime resumen ejecutivo."""
    print("\n╔══════════════════════════════════════════════════════════════════╗")
    print("║  RESUMEN EJECUTIVO                                               ║")
    print("╚══════════════════════════════════════════════════════════════════╝\n")
    
    passed = [r for r in results if evaluate_result(r) == "PASS"]
    failed = [r for r in results if evaluate_result(r) == "FAIL"]
    warned = [r for r in results if evaluate_result(r) == "WARN"]
    errors = [r for r in results if evaluate_result(r) == "ERROR"]
    
    # One-liner estilo solicitado
    summary_parts = []
    for r in results:
        status = evaluate_result(r)
        if status == "PASS":
            summary_parts.append(f"{r.compiler}: PASS ({r.balance_score:.0f}%)")
        elif status == "FAIL":
            summary_parts.append(f"{r.compiler}: FAIL ({r.balance_score:.0f}%)")
        elif status == "WARN":
            summary_parts.append(f"{r.compiler}: WARN ({r.balance_score:.0f}%)")
        else:
            summary_parts.append(f"{r.compiler}: ERROR")
    
    print("  " + " | ".join(summary_parts))
    
    print(f"\n  Total: {len(passed)} PASS, {len(warned)} WARN, {len(failed)} FAIL, {len(errors)} ERROR")
    
    if failed:
        print("\n  ⚠️  ALERTA: Los siguientes compiladores ROMPEN la defensa:")
        for r in failed:
            print(f"     - {r.compiler}: Balance {r.balance_score:.1f}% (esperado >50%)")
        print("\n  Posibles causas:")
        print("     1. Loop Invariant Code Motion (LICM) saca lecturas del bucle")
        print("     2. Optimizaciones agresivas en E-cores de CPUs híbridas")
        print("     3. Reordenamiento de memory_order_relaxed")
        
        print("\n  Fixes recomendados:")
        print("     1. Usar -fno-tree-loop-im para deshabilitar LICM")
        print("     2. Añadir compiler barriers en cached_load_stats.hpp")
        print("     3. Cambiar a memory_order_acquire en lecturas críticas")
    
    if passed:
        print("\n  ✅ Compiladores que PRESERVAN la defensa:")
        for r in passed:
            print(f"     - {r.compiler}: Balance {r.balance_score:.1f}%")


def main():
    parser = argparse.ArgumentParser(
        description="Compiler Battle: Test Hash Flooding Defense across compilers"
    )
    parser.add_argument(
        "--compilers", "-c",
        help="Comma-separated list of compiler configs to test (default: auto-detect)",
        default=None
    )
    parser.add_argument(
        "--runs", "-r",
        type=int,
        default=1,
        help="Number of runs per compiler (default: 1)"
    )
    parser.add_argument(
        "--timeout", "-t",
        type=int,
        default=120,
        help="Timeout per benchmark run in seconds (default: 120)"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print raw benchmark output"
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available compiler configurations"
    )
    
    args = parser.parse_args()
    
    # Rutas del proyecto
    project_root = Path(__file__).parent.absolute()
    source_file = project_root / "bench" / "adversarial_bench.cpp"
    include_dir = project_root / "include"
    build_dir = project_root / "build_battle"
    
    if args.list:
        print("\nCompiler configurations disponibles:")
        available = find_available_compilers()
        for key, config in COMPILER_CONFIGS.items():
            status = "✅" if key in available else "❌ (not found)"
            print(f"  {key:<20} {status}")
            print(f"      {config.description}")
        return 0
    
    print_banner()
    
    # Verificar que existe el source
    if not source_file.exists():
        print(f"❌ ERROR: No se encuentra {source_file}")
        return 1
    
    # Crear directorio de build
    build_dir.mkdir(exist_ok=True)
    
    # Determinar qué compiladores usar
    if args.compilers:
        compiler_keys = [c.strip() for c in args.compilers.split(",")]
        # Validar
        for key in compiler_keys:
            if key not in COMPILER_CONFIGS:
                print(f"❌ ERROR: Compiler config '{key}' no existe")
                print(f"   Disponibles: {', '.join(COMPILER_CONFIGS.keys())}")
                return 1
    else:
        # Auto-detect: probar gcc-O3, gcc-O2, gcc-O3-no-licm, y cualquier otro disponible
        available = find_available_compilers()
        compiler_keys = [k for k in ["gcc-O3", "gcc-O2", "gcc-O3-no-licm"] if k in available]
        
        # Añadir Intel si está disponible
        if "icpx" in available:
            compiler_keys.append("icpx")
        elif "icx" in available:
            compiler_keys.append("icx")
        
        if not compiler_keys:
            print("❌ ERROR: No se encontró ningún compilador compatible")
            print("   Instale g++ o configure el PATH correctamente")
            return 1
    
    print(f"📋 Compiladores a probar: {', '.join(compiler_keys)}")
    print(f"📋 Runs por compilador: {args.runs}")
    print(f"📋 Timeout: {args.timeout}s\n")
    
    all_results: List[BenchResult] = []
    
    for key in compiler_keys:
        config = COMPILER_CONFIGS[key]
        print(f"\n{'─' * 60}")
        print(f"🔨 Compilando con {config.name}...")
        print(f"   {config.description}")
        print(f"   Flags: {' '.join(config.flags)}")
        
        binary_path = build_dir / f"adversarial_{key.replace('-', '_')}"
        if sys.platform == "win32":
            binary_path = binary_path.with_suffix(".exe")
        
        # Compilar
        success, msg = compile_benchmark(config, source_file, binary_path, include_dir)
        
        if not success:
            print(f"   ❌ Compilación falló: {msg}")
            result = BenchResult(
                compiler=config.name, balance_score=0, hotspot_detected=False,
                suspicious_patterns=0, blocked_redirects=0, execution_time_ms=0,
                raw_output="", success=False, error=msg
            )
            all_results.append(result)
            continue
        
        print(f"   ✅ Compilación exitosa")
        
        # Ejecutar benchmark (múltiples runs si se solicita)
        run_results = []
        for run_num in range(args.runs):
            if args.runs > 1:
                print(f"   🏃 Ejecutando run {run_num + 1}/{args.runs}...")
            else:
                print(f"   🏃 Ejecutando benchmark...")
            
            result = run_benchmark(binary_path, args.timeout)
            result.compiler = config.name
            run_results.append(result)
            
            if args.verbose and result.success:
                print("\n" + "=" * 40 + " RAW OUTPUT " + "=" * 40)
                print(result.raw_output)
                print("=" * 92)
        
        # Si hay múltiples runs, usar el peor caso (mínimo balance)
        if run_results:
            worst = min(run_results, key=lambda r: r.balance_score if r.success else float('inf'))
            all_results.append(worst)
            
            status = evaluate_result(worst)
            icon = {"PASS": "✅", "WARN": "⚠️", "FAIL": "❌", "ERROR": "💥"}.get(status, "?")
            print(f"   {icon} Balance Score: {worst.balance_score:.1f}% -> {status}")
    
    # Imprimir resultados
    print_result_table(all_results)
    print_summary(all_results)
    
    # Cleanup
    print(f"\n💾 Binarios guardados en: {build_dir}")
    
    # Exit code: 1 si hay FAILs, 0 si todo PASS/WARN
    has_failures = any(evaluate_result(r) == "FAIL" for r in all_results)
    return 1 if has_failures else 0


if __name__ == "__main__":
    sys.exit(main())
