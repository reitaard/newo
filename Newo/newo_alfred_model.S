.section .rodata.newo_alfred_model,"a",@progbits
.balign 16
.global newo_alfred_model_start
.global newo_alfred_model_end

newo_alfred_model_start:
.incbin "Newo/wakeword/alfred.tflite"
newo_alfred_model_end:
