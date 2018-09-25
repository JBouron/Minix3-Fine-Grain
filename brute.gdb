set logging on
watch big_kernel_lock
display __cpu_local_vars[0].proc_ptr
display __cpu_local_vars[0].proc_ptr->p_name
continue
b *0xf041bda4
b *0xf041bdd4
del 1
define brute
	print $arg0
	while 1
		continue
		bt full
		set print repeats 0
		print "##############################################################################"
		set print repeats 10
	end
end
brute 0
