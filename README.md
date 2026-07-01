сборка:

1- запустить  C:\w64devkit\w64devkit\w64devkit.exe
2-в открывшемся терминале:
  a- cd C:/Users/andrey.danilov/Documents/OutOfProjects/Скришнотер/vrmshot_v3_bottom_to_top 
  b- windres vrmshot_v3_btt.rc -o vrmshot_v3_btt_res.o
  c- gcc -O2 -o vrmshot_v3_bottom_to_top.exe vrmshot_v3_bottom_to_top.c vrmshot_v3_btt_res.o -lgdi32 -luser32 -lcomctl32 -mwindows -static
3- запустить exe
