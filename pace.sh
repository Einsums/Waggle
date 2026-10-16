#!/bin/bash
#SBATCH -J waggle-calibrate
#SBATCH -A gts-cs207
#SBATCH -q embers
#SBATCH -p gpu-rtxpro-blackwell,gpu-l40s,gpu-h100,gpu-h200,gpu-a100
#SBATCH -N 1
#SBATCH --exclusive
#SBATCH -t 00:10:00
#SBATCH -o waggle-calibrate-%j.out

module purge
module load miniforge/24.3.0-0
eval "$(conda shell.bash hook)"
conda activate waggle-dev
cd $HOME/git/Waggle
PYTHONPATH=build/python python -m waggle calibrate

