p Kernel.private_instance_methods(false).include?(:__frame_locals)
p Kernel.public_methods(false).include?(:__frame_locals)
x = (Kernel.__frame_locals rescue $!.class)
p x
p Kernel.private_instance_methods(false).include?(:eval)
p Kernel.public_methods(false).include?(:eval)
p Kernel.private_instance_methods(false).include?(:binding)
p Kernel.public_methods(false).include?(:binding)
