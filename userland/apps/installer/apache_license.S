/* Embed the complete Apache License text from the repository. */
.section .rodata.installer_license,"a",@progbits
.global installer_license_text
.hidden installer_license_text
.type installer_license_text,@object
installer_license_text:
.incbin INSTALLER_LICENSE_PATH
.byte 0
.size installer_license_text, .-installer_license_text

.section .note.GNU-stack,"",@progbits
