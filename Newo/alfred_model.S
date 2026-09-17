.section .rodata.alfred_model,"a",@progbits
.balign 16
.global newo_alfred_tflite_start
.global newo_alfred_tflite_end
newo_alfred_tflite_start:
.incbin "wakeword/alfred.tflite"
newo_alfred_tflite_end:
.balign 16
