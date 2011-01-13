#/bin/bash
# get running time
ls $2/*.time   | perl rmark-time.pl > $2/$2.time
# get MER
cat $2/*out | sort -g | perl rmark-mer.pl $1.pos $2/$2.time > $2/$2.mer
# get ROC
cat $2/*out | sort -g | rmark-rocplot -N 5000 --seed 181 $1 - > $2/$2.xy
# get mer from rmark-rocplot
cat $2/*out | sort -g | rmark-rocplot -N 5000 --mer --seed 181 $1 - > $2/$2.bmer

# copy files to cwd
cp $2/$2.mer ./
cp $2/$2.bmer ./
cp $2/$2.time ./
cp $2/$2.xy ./

# summarize files to stdout
echo -n $2 | awk '{printf("%-50s", $0)}' > $2/$2.sum
cat $2/$2.bmer | awk '{printf("  %5s  %5s  %5s  %5s   ", $3, $7, $11, $15)}' >> $2/$2.sum
grep ummary $2/$2.mer | awk '{print $7}' >> $2/$2.sum
cp $2/$2.sum ./
cat $2.sum
