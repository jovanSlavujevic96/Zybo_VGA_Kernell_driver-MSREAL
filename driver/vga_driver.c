#include <linux/kernel.h>

#include <linux/string.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/device.h>

#include <linux/io.h> //iowrite ioread
#include <linux/slab.h>//kmalloc kfree
#include <linux/platform_device.h>//platform driver
#include <linux/of.h>//of_match_table
#include <linux/ioport.h>//ioremap

#include <linux/dma-mapping.h>  //dma access
#include <linux/mm.h>  //dma access
#include <linux/interrupt.h>  //interrupt handlers

#include "letters.h"
#include "small_letters.h"

MODULE_AUTHOR ("FTN");
MODULE_DESCRIPTION("Test Driver for VGA controller IP.");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("custom:vga_dma controller");

#define DEVICE_NAME "vga_dma"
#define DRIVER_NAME "vga_dma_driver"
#define BUFF_SIZE 50
#define MAX_PKT_LEN 640*480*4
#define MAX_W 639
#define MAX_H 479

#define BIG_FONT_W 10
#define BIG_FONT_H 14
#define SMALL_FONT_W 5
#define SMALL_FONT_H 7

typedef int state_t;
enum {state_TEXT, state_LINE, state_RECT, state_CIRC, state_PIX, state_ERR};

//*******************FUNCTION PROTOTYPES************************************
static int vga_dma_probe(struct platform_device *pdev);
static int vga_dma_open(struct inode *i, struct file *f);
static int vga_dma_close(struct inode *i, struct file *f);
static ssize_t vga_dma_read(struct file *f, char __user *buf, size_t len, loff_t *off);
static ssize_t vga_dma_write(struct file *f, const char __user *buf, size_t length, loff_t *off);
static ssize_t vga_dma_mmap(struct file *f, struct vm_area_struct *vma_s);
static int __init vga_dma_init(void);
static void __exit vga_dma_exit(void);
static int vga_dma_remove(struct platform_device *pdev);

static irqreturn_t dma_isr(int irq,void*dev_id);
int dma_init(void __iomem *base_address);
u32 dma_simple_write(dma_addr_t TxBufferPtr, u32 max_pkt_len, void __iomem *base_address); 

//*********************GLOBAL VARIABLES*************************************
struct vga_dma_info {
  unsigned long mem_start;
  unsigned long mem_end;
  void __iomem *base_addr;
  int irq_num;
};

static struct cdev *my_cdev;
static dev_t my_dev_id;
static struct class *my_class;
static struct device *my_device;
static struct vga_dma_info *vp = NULL;

static struct file_operations my_fops =
{
	.owner = THIS_MODULE,
	.open = vga_dma_open,
	.release = vga_dma_close,
	.read = vga_dma_read,
	.write = vga_dma_write,
	.mmap = vga_dma_mmap
};

static struct of_device_id vga_dma_of_match[] = {
	{ .compatible = "xlnx,axi-dma-mm2s-channel", },
	{ .compatible = "vga_dma"},
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, vga_dma_of_match);

static struct platform_driver vga_dma_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table	= vga_dma_of_match,
	},
	.probe		= vga_dma_probe,
	.remove	= vga_dma_remove,
};


dma_addr_t tx_phy_buffer;
u32 *tx_vir_buffer;
static const bool(* b_ptr)[7][5] = NULL;
static unsigned long long small_letter[7][5] = {{0}};
static unsigned long long big_letter[14][10] = {{0}};

static const bool(*const (letter_forms[]))[7][5] = 
{
	&A,&B,&C,&D,&E,&F,&G,&H,&I,&J,&K,&L,&M,&N,&O,&P,&Q,&R,&S,&T,&U,&V,&W,&X,&Y,&Z,
	&a,&b,&c,&d,&e,&f,&g,&h,&i,&j,&k,&l,&m,&n,&o,&p,&q,&r,&s,&t,&u,&v,&w,&x,&y,&z,
	&comma,&dot,&space,&questionnaire,&exclamation
};
static const char letters[] =
{
	'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
	'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z',
	',','.',' ','?','!'
};


//***************************************************************************
// PROBE AND REMOVE
static int vga_dma_probe(struct platform_device *pdev)
{
	struct resource *r_mem;
	int rc = 0;

	printk(KERN_INFO "vga_dma_probe: Probing\n");
	// Get phisical register adress space from device tree
	r_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r_mem) {
		printk(KERN_ALERT "vga_dma_probe: Failed to get reg resource\n");
		return -ENODEV;
	}
	// Get memory for structure vga_dma_info
	vp = (struct vga_dma_info *) kmalloc(sizeof(struct vga_dma_info), GFP_KERNEL);
	if (!vp) {
		printk(KERN_ALERT "vga_dma_probe: Could not allocate memory for structure vga_dma_info\n");
		return -ENOMEM;
	}
	// Put phisical adresses in timer_info structure
	vp->mem_start = r_mem->start;
	vp->mem_end = r_mem->end;

	// Reserve that memory space for this driver
	if (!request_mem_region(vp->mem_start,vp->mem_end - vp->mem_start + 1, DRIVER_NAME))
	{
		printk(KERN_ALERT "vga_dma_probe: Could not lock memory region at %p\n",(void *)vp->mem_start);
		rc = -EBUSY;
		goto error1;
	}    
	// Remap phisical to virtual adresses

	vp->base_addr = ioremap(vp->mem_start, vp->mem_end - vp->mem_start + 1);
	if (!vp->base_addr) {
		printk(KERN_ALERT "vga_dma_probe: Could not allocate memory for remapping\n");
		rc = -EIO;
		goto error2;
	}

	// Get irq num 
	vp->irq_num = platform_get_irq(pdev, 0);
	if(!vp->irq_num)
	{
		printk(KERN_ERR "vga_dma_probe: Could not get IRQ resource\n");
		rc = -ENODEV;
		goto error2;
	}

	if (request_irq(vp->irq_num, dma_isr, 0, DEVICE_NAME, NULL)) {
		printk(KERN_ERR "vga_dma_probe: Could not register IRQ %d\n", vp->irq_num);
		return -EIO;
		goto error3;
	}
	else {
		printk(KERN_INFO "vga_dma_probe: Registered IRQ %d\n", vp->irq_num);
	}

	/* INIT DMA */
	dma_init(vp->base_addr);
	dma_simple_write(tx_phy_buffer, MAX_PKT_LEN, vp->base_addr); // helper function, defined later

	printk(KERN_NOTICE "vga_dma_probe: VGA platform driver registered\n");
	return 0;//ALL OK

error3:
	iounmap(vp->base_addr);
error2:
	release_mem_region(vp->mem_start, vp->mem_end - vp->mem_start + 1);
	kfree(vp);
error1:
	return rc;

}

static int vga_dma_remove(struct platform_device *pdev)
{
	u32 reset = 0x00000004;
	// writing to MM2S_DMACR register. Seting reset bit (3. bit)
	printk(KERN_INFO "vga_dma_probe: resseting");
	iowrite32(reset, vp->base_addr); 

	free_irq(vp->irq_num, NULL);
	iounmap(vp->base_addr);
	release_mem_region(vp->mem_start, vp->mem_end - vp->mem_start + 1);
	kfree(vp);
	printk(KERN_INFO "vga_dma_probe: VGA DMA removed");
	return 0;
}

//***************************************************
// IMPLEMENTATION OF FILE OPERATION FUNCTIONS
static int vga_dma_open(struct inode *i, struct file *f)
{
	printk(KERN_INFO "vga_dma opened\n");
	return 0;
}

static int vga_dma_close(struct inode *i, struct file *f)
{
	printk(KERN_INFO "vga_dma closed\n");
	return 0;
}

static ssize_t vga_dma_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	printk(KERN_INFO "vga_dma read\n");
	return 0;
}

static void Word_onScreen(const bool big_font, const unsigned int x_StartPos, const unsigned int y_StartPos, const unsigned int x_Step, const unsigned int y_Step, const unsigned long long Col_Bckg)
{
	unsigned int x,y,i,j;
	for(y=y_StartPos, i=0; i<y_Step; ++i,++y)
		for(x=x_StartPos, j=0; j<x_Step; ++x,++j)
		{
			u32 rgb = (big_font == true) ? ((u32)big_letter[i][j]) : ((u32)small_letter[i][j]);
			tx_vir_buffer[640*y + x] = rgb;
		}
	x=x_StartPos+x_Step;
	for(y=y_StartPos,i=0; i<y_Step; ++i,++y)
	{
		tx_vir_buffer[640*y + x] = (u32)Col_Bckg;
	}
}

static void parse_buffer(const char* buffer, char(* commands)[BUFF_SIZE])
{
	int i, incr=0, len=0;
	for(i=0;i<strlen(buffer);i++)
	{
		if(buffer[i] != ';' && buffer[i] != '\n')
			commands[incr][i-len] = buffer[i];
		else if(buffer[i] == ';')
		{
			len += strlen(commands[incr]) + 1;
			incr++;
		}
		else if(buffer[i] == '\n')
		{
			break;
		}
	}
}

static unsigned int strToInt(const char* string_num)
{
	int i,dec=1;
	unsigned int val=0;
	for(i=strlen(string_num)-1;i>=0;--i)
	{
		unsigned int tmp = (string_num[i]-48)*dec;
		dec *= 10;
		val += tmp;
	}
	return val;
}

static state_t getState(const char* command0)
{
	if(!strcmp(command0,"TEXT") || !strcmp(command0,"text") )
		return state_TEXT;
	else if(!strcmp(command0,"LINE") || !strcmp(command0,"line") )
		return state_LINE;
	else if(!strcmp(command0,"RECT") || !strcmp(command0,"rect") )
		return state_RECT;
	else if(!strcmp(command0,"CIRC") || !strcmp(command0,"circ") )
		return state_CIRC;
	else if(!strcmp(command0,"PIX")  || !strcmp(command0,"pix" ) )
		return state_PIX;
	printk(KERN_ERR "%s is not appropriate command\n",command0);
	return state_ERR;
}

struct Text
{
	char m_Letters[BUFF_SIZE];
	bool m_BigFont;
	unsigned int m_Xstart, m_Ystart;
	unsigned long long m_ColorLetter, m_ColorBckg;
};

static void printText(const struct Text* text)
{
	printk("Text struct info:\n");
	printk("letters: %s\n",text->m_Letters);
	printk("big font: %s\n", (text->m_BigFont == true) ? "true" : "false");
	printk("x: %d , y: %d\n", text->m_Xstart, text->m_Ystart);
	printk("letter col: %llu, bckg col: %llu\n", text->m_ColorLetter, text->m_ColorBckg);
}

static void initText(struct Text* text)
{
	int i;
	for(i=0;i<BUFF_SIZE;++i)
		text->m_Letters[i] = 0;
	text->m_BigFont = false;
	text->m_Xstart = 0, text->m_Ystart=0;
	text->m_ColorLetter=0, text->m_ColorBckg=0;
}

static int setText(struct Text* text, const char(* commands)[BUFF_SIZE])
{
	int i;
	for(i=0;i<strlen(commands[1]);++i)
		text->m_Letters[i] = commands[1][i];
	
	if(!strcmp(commands[2],"big") || !strcmp(commands[2],"BIG") )
		text->m_BigFont = true;
	else if(!strcmp(commands[2],"small") || !strcmp(commands[2],"SMALL") )
		text->m_BigFont = false;
	else
	{
		printk(KERN_ERR "%s this is not appropriate command\n",commands[2]);
		return -1;
	}	
	text->m_Xstart = strToInt(commands[3]);
	text->m_Ystart = strToInt(commands[4]);
	i=kstrtoull((unsigned char*)commands[5],0,&text->m_ColorLetter);
	i=kstrtoull((unsigned char*)commands[6],0,&text->m_ColorBckg);
	return 0;
}

static int check_character(const char letter)
{
	if( !(letter >= 'A' && letter <= 'Z') && !(letter >= 'a' && letter <= 'z') &&
		letter != ' ' && letter != '!' && letter != ',' && letter != '?' && letter != '.')
	{
		return -1;
	}
	return 0;
}

static void set_character(const char letter, const bool(** ptr)[7][5])
{
	int i;
	for(i=0;i<57;++i)
		if(letter == letters[i])
		{
			*ptr = letter_forms[i];
			return;
		}	
}

static void DoubleSizeMat(void)
{
	int i,j,c,d;
	for(i=0;i<7;++i)
		for(j=0;j<5;j++)
			for(c=0;c<2;c++)
				for(d=0;d<2;d++)
					big_letter[2*i+c][2*j+d]=small_letter[i][j];
}

static void assignValToCharacter(const bool big_letter, const unsigned long long color_letter, const unsigned long long color_bckg)
{
	int i,j;
	for(i=0;i<7;++i)
		for(j=0;j<5;++j)
		{
			small_letter[i][j] = ((*b_ptr)[i][j] == 1) ? color_bckg : color_letter;
		}
	if(big_letter)
	{
		DoubleSizeMat();
	}
}

static int printWord(const struct Text* text)
{
	unsigned int i, Y = text->m_Ystart, X=text->m_Xstart, strLen = strlen(text->m_Letters),
	x_step = (text->m_BigFont == true) ? BIG_FONT_W : SMALL_FONT_W,
	y_step = (text->m_BigFont == true) ? BIG_FONT_H : SMALL_FONT_H,
	checkX = X + x_step, checkY = Y + y_step;
	bool error=false;
	for(i=0; i<strLen; ++i)
	{
		if(check_character(text->m_Letters[i]) == -1)
		{
			printk(KERN_ERR "VGA_DMA: %c cant be printed on screen, there's not this character on our library!\n",text->m_Letters[i] );
			error = true;	
		}
	}

	if(checkX > MAX_W || checkY > MAX_H)
	{
		printk(KERN_ERR "VGA_DMA: %s cant whole fit into screen by one or both axis!\n",text->m_Letters);
		error = true;
	}

	if(error)
		return -1;

	for(i=0;i<strLen;++i)
	{
		set_character(text->m_Letters[i], &b_ptr);
		assignValToCharacter(text->m_BigFont, text->m_ColorLetter, text->m_ColorBckg);
		Word_onScreen(text->m_BigFont, X, Y, x_step, y_step, text->m_ColorBckg);
		X += x_step+1;
		b_ptr = NULL;
		if(X+x_step > MAX_W && i < strLen-1)
		{
			printk(KERN_ERR "VGA_DMA: %c cant whole fit into screen by x axis!\n",text->m_Letters[i]);
			break;
		}
	}
	return 0;
}

struct Line
{
	unsigned int m_Xstart, m_Ystart;
	unsigned int m_Xend, m_Yend;
	unsigned long long m_LineColor;
	bool m_HorizontalLine;
};

void printLine(const struct Line* line)
{
	printk("Line info:\n");
	printk("(%d,%d) <-> (%d,%d)\n",line->m_Xstart, line->m_Ystart, line->m_Xend, line->m_Xstart);
	printk("line color: %llu\n", line->m_LineColor);
	printk("line: %s\n", (line->m_HorizontalLine == true) ? "Horizontal" : "Vertical");
}

int setLine(struct Line* line, const char(* commands)[BUFF_SIZE] )
{
	int ret;
	line->m_Xstart = strToInt(commands[1]);
	line->m_Ystart = strToInt(commands[2]);
	line->m_Xend   = strToInt(commands[3]);
	line->m_Yend   = strToInt(commands[4]);
	ret = kstrtoull((unsigned char*)commands[5],0,&line->m_LineColor);
	
	if(line->m_Ystart == line->m_Yend)
		line->m_HorizontalLine = true;
	else if(line->m_Xstart == line->m_Xend)
		line->m_HorizontalLine = false;
	else
	{
		printk(KERN_ERR "VGA_DMA: Line is nor horisontal nor vertical!\n");
		return -1;
	}
	return 0;
}

void Line_onScreen(const struct Line* line)
{
	int i,start,end;
	if(line->m_HorizontalLine)
	{
		if(line->m_Xend > line->m_Xstart)
			start = line->m_Xstart,
			end = line->m_Xend;
		else
			start = line->m_Xend,
			end = line->m_Xstart;

		for(i=start; i<=end; ++i)
			tx_vir_buffer[640*line->m_Ystart + i] = (u32)line->m_LineColor;
		return;
	}
	if(line->m_Yend > line->m_Ystart)
		start = line->m_Ystart,
		end = line->m_Yend;
	else
		start = line->m_Yend,
		end = line->m_Ystart;
	
	for(i=start; i<=end; ++i)
		tx_vir_buffer[640*i + line->m_Xstart] = (u32)line->m_LineColor;
}

struct Rect
{
	unsigned int m_XtopLeft, m_YtopLeft;
	unsigned int m_XbottomRight, m_YbottomRight;
	unsigned long long m_RectColor;
	bool m_FillRect;
};


void printRect(const struct Rect* rect)
{
	printk("Rect info:\n");
	printk("(%d,%d) <-> (%d,%d)\n",rect->m_XtopLeft, rect->m_YtopLeft, rect->m_XbottomRight, rect->m_YbottomRight);
	printk("rect color: %llu\n", rect->m_RectColor);
	printk("fill rect: %s\n", (rect->m_FillRect == true) ? "true" : "false");
}

int setRect(struct Rect* rect, const char(* commands)[BUFF_SIZE] )
{
	int ret;
	rect->m_XtopLeft = strToInt(commands[1]);
	rect->m_YtopLeft = strToInt(commands[2]);
	rect->m_XbottomRight = strToInt(commands[3]);
	rect->m_YbottomRight = strToInt(commands[4]);
	ret = kstrtoull((unsigned char*)commands[5],0,&rect->m_RectColor);
	
	if(!strcmp(commands[6],"FILL") || !strcmp(commands[6],"fill"))
		rect->m_FillRect = true;
	else if(!strcmp(commands[6],"NO") || !strcmp(commands[6],"no"))
		rect->m_FillRect = false;
	else
	{
		printk(KERN_ERR "%s -> incorrect command!\n",commands[6]);
		return -1;
	}
	return 0;
}

void Rect_onScreen(const struct Rect* rect)
{
	int i,j;
	if(!rect->m_FillRect)
	{
		struct Line lines[4] = 
		{ 
			{rect->m_XtopLeft, rect->m_YtopLeft, rect->m_XbottomRight, rect->m_YtopLeft, rect->m_RectColor, true},
			{rect->m_XtopLeft, rect->m_YtopLeft, rect->m_XtopLeft, rect->m_YbottomRight, rect->m_RectColor, false},
			{rect->m_XtopLeft, rect->m_YbottomRight, rect->m_XbottomRight, rect->m_YbottomRight, rect->m_RectColor, true},
			{rect->m_XbottomRight, rect->m_YtopLeft, rect->m_XbottomRight, rect->m_YbottomRight, rect->m_RectColor, false}
		};
		for(i=0;i<4;i++)
			Line_onScreen(&lines[i]);
		return;
	}
	for(i=rect->m_XtopLeft; i<=rect->m_XbottomRight; ++i)
		for(j=rect->m_YtopLeft; j<=rect->m_YbottomRight; ++j)
			tx_vir_buffer[640*j + i] = (u32)rect->m_RectColor;
}
/*
float SQRT(float num)
{
	float sol;
	if(num >= 1)
	{
		for(sol=1;sol<=num;sol += 0.01)
		{
			if(sol*sol >= num-0.1 && sol*sol <= num)
				return sol;
		}
	}
	for(sol=0; sol <= num; sol += 0.001)
	{
		if(sol*sol >= num-0.001)
			return sol;
	}
}

struct Circle
{
	unsigned int m_Cx, m_Cy;
	unsigned int m_r;
	unsigned long long m_CircleColor;
	bool m_FillCircle;
};

void setCircle(struct Circle* circle, const char(* commands)[BUFF_SIZE])
{
	int ret;
	circle->m_Cx = strToInt(commands[1]);
	circle->m_Cy = strToInt(commands[2]);
	circle->m_r = strToInt(commands[3]);
	ret = kstrtoull((unsigned char*)commands[4],0, &circle->m_CircleColor);
	if(ret)
		return;
	circle->m_FillCircle = (!strcmp(commands[5],"fill") || !strcmp(commands[5],"FILL")) ? true : false;
}

void draw_circle(const struct Circle* circ)
{
	unsigned int xx = circ->m_Cx - circ->m_r, x,y;
	unsigned int yy = circ->m_Cy - circ->m_r;
	float q;
	int XX = circ->m_Cx - circ->m_r, YY = circ->m_Cy - circ->m_r;
	if(XX < 0 || YY < 0)
	{
		printk(KERN_ERR "can fit whole on screen!\n");
		return;
	}

	for(x = xx; x <= (xx+2*circ->m_r); ++x)
		for(y = yy; y <= (yy+2*circ->m_r); ++y)
		{
			bool info;
			q = SQRT((float)((circ->m_Cx-x)*(circ->m_Cx-x)+(circ->m_Cy-y)*(circ->m_Cy-y) )) ;
			info = (circ->m_FillCircle == true) ? ( (circ->m_r*0.9 > q) ? true : false ) : ((circ->m_r*0.9 > q && circ->m_r*0.8 < q) ? true : false);
			if(info)	
			{
				tx_vir_buffer[640*y + x] = (u32) circ->m_CircleColor;
			}
		}
}
*/
static int assign_params_from_commands(const state_t state, const char(* commands)[BUFF_SIZE])
{
	int ret=0;
	if(state == state_TEXT)
	{
		struct Text text;
		initText(&text);
		ret = setText(&text, commands);
		if(ret == -1)
			return ret;
		printText(&text);
		ret = printWord(&text);
	}
	else if(state == state_LINE)
	{
		struct Line line;
		ret = setLine(&line, commands);
		if(ret == -1)
			return ret;
		printLine(&line);
		Line_onScreen(&line);
	}
	else if(state == state_RECT)
	{
		struct Rect rect;
		ret = setRect(&rect, commands);
		if(ret == -1)
			return ret;
		printRect(&rect);
		Rect_onScreen(&rect);
	}
	/*
	else if(state == state_CIRC)
	{
		struct Circle circ;
		setCircle(&circ, commands);
		draw_circle(&circ);
	}
	*/
	return ret;
}

static ssize_t vga_dma_write(struct file *f, const char __user *buf, size_t length, loff_t *off)
{	
	char buff[2*BUFF_SIZE];
	int ret = 0;
	char commands[7][BUFF_SIZE] = {{0}};
	state_t state;
	int i;
	
	printk("\n");

	ret = copy_from_user(buff, buf, length);  
	if(ret){
		printk("copy from user failed \n");
		return -EFAULT;
	}  
	buff[length] = '\0';
	
	parse_buffer(buff, commands);
	for(i=0; i<7;++i)
		printk("%d: %s\n", i, commands[i]);

	state = getState(commands[0]);
	ret = assign_params_from_commands(state, commands);

	return length;
}

static ssize_t vga_dma_mmap(struct file *f, struct vm_area_struct *vma_s)
{
	int ret = 0;
	long length = vma_s->vm_end - vma_s->vm_start;

	//printk(KERN_INFO "DMA TX Buffer is being memory mapped\n");

	if(length > MAX_PKT_LEN)
	{
		return -EIO;
		printk(KERN_ERR "Trying to mmap more space than it's allocated\n");
	}

	ret = dma_mmap_coherent(NULL, vma_s, tx_vir_buffer, tx_phy_buffer, length);
	if(ret<0)
	{
		printk(KERN_ERR "memory map failed\n");
		return ret;
	}
	return 0;
}

/****************************************************/
// IMPLEMENTATION OF DMA related functions

static irqreturn_t dma_isr(int irq,void*dev_id)
{
	u32 IrqStatus;  
	/* Read pending interrupts */
	IrqStatus = ioread32(vp->base_addr + 4);//read irq status from MM2S_DMASR register
	iowrite32(IrqStatus | 0x00007000, vp->base_addr + 4);//clear irq status in MM2S_DMASR register
	//(clearing is done by writing 1 on 13. bit in MM2S_DMASR (IOC_Irq)

	/*Send a transaction*/
	dma_simple_write(tx_phy_buffer, MAX_PKT_LEN, vp->base_addr); //My function that starts a DMA transaction
	return IRQ_HANDLED;;
}

int dma_init(void __iomem *base_address)
{
	u32 reset = 0x00000004;
	u32 IOC_IRQ_EN; 
	u32 ERR_IRQ_EN;
	u32 MM2S_DMACR_reg;
	u32 en_interrupt;

	IOC_IRQ_EN = 1 << 12; // this is IOC_IrqEn bit in MM2S_DMACR register
	ERR_IRQ_EN = 1 << 14; // this is Err_IrqEn bit in MM2S_DMACR register

	iowrite32(reset, base_address); // writing to MM2S_DMACR register. Seting reset bit (3. bit)

	MM2S_DMACR_reg = ioread32(base_address); // Reading from MM2S_DMACR register inside DMA
	en_interrupt = MM2S_DMACR_reg | IOC_IRQ_EN | ERR_IRQ_EN;// seting 13. and 15.th bit in MM2S_DMACR
	iowrite32(en_interrupt, base_address); // writing to MM2S_DMACR register  
	return 0;
}

u32 dma_simple_write(dma_addr_t TxBufferPtr, u32 max_pkt_len, void __iomem *base_address) {
	u32 MM2S_DMACR_reg;

	MM2S_DMACR_reg = ioread32(base_address); // READ from MM2S_DMACR register

	iowrite32(0x1 |  MM2S_DMACR_reg, base_address); // set RS bit in MM2S_DMACR register (this bit starts the DMA)

	iowrite32((u32)TxBufferPtr, base_address + 24); // Write into MM2S_SA register the value of TxBufferPtr.
	// With this, the DMA knows from where to start.

	iowrite32(max_pkt_len, base_address + 40); // Write into MM2S_LENGTH register. This is the length of a tranaction.
	// In our case this is the size of the image (640*480*4)
	return 0;
}



//***************************************************
// INIT AND EXIT FUNCTIONS OF THE DRIVER

static int __init vga_dma_init(void)
{

	int ret = 0;
	int i = 0;

	printk(KERN_INFO "vga_dma_init: Initialize Module \"%s\"\n", DEVICE_NAME);
	ret = alloc_chrdev_region(&my_dev_id, 0, 1, "VGA_region");
	if (ret)
	{
		printk(KERN_ALERT "vga_dma_init: Failed CHRDEV!\n");
		return -1;
	}
	printk(KERN_INFO "vga_dma_init: Successful CHRDEV!\n");
	my_class = class_create(THIS_MODULE, "VGA_drv");
	if (my_class == NULL)
	{
		printk(KERN_ALERT "vga_dma_init: Failed class create!\n");
		goto fail_0;
	}
	printk(KERN_INFO "vga_dma_init: Successful class chardev1 create!\n");
	my_device = device_create(my_class, NULL, MKDEV(MAJOR(my_dev_id),0), NULL, "vga_dma");
	if (my_device == NULL)
	{
		goto fail_1;
	}

	printk(KERN_INFO "vga_dma_init: Device created\n");

	my_cdev = cdev_alloc();	
	my_cdev->ops = &my_fops;
	my_cdev->owner = THIS_MODULE;
	ret = cdev_add(my_cdev, my_dev_id, 1);
	if (ret)
	{
		printk(KERN_ERR "vga_dma_init: Failed to add cdev\n");
		goto fail_2;
	}
	printk(KERN_INFO "vga_dma_init: Module init done\n");

	tx_vir_buffer = dma_alloc_coherent(NULL, MAX_PKT_LEN, &tx_phy_buffer, GFP_DMA | GFP_KERNEL);
	if(!tx_vir_buffer){
		printk(KERN_ALERT "vga_dma_init: Could not allocate dma_alloc_coherent for img");
		goto fail_3;
	}
	else
		printk("vga_dma_init: Successfully allocated memory for dma transaction buffer\n");
	for (i = 0; i < MAX_PKT_LEN/4;i++)
		tx_vir_buffer[i] = 0x00000000;
	printk(KERN_INFO "vga_dma_init: DMA memory reset.\n");
	return platform_driver_register(&vga_dma_driver);

fail_3:
	cdev_del(my_cdev);
fail_2:
	device_destroy(my_class, MKDEV(MAJOR(my_dev_id),0));
fail_1:
	class_destroy(my_class);
fail_0:
	unregister_chrdev_region(my_dev_id, 1);
	return -1;

} 

static void __exit vga_dma_exit(void)  		
{
	//Reset DMA memory
	int i =0;
	for (i = 0; i < MAX_PKT_LEN/4; i++) 
		tx_vir_buffer[i] = 0x00000000;
	printk(KERN_INFO "vga_dma_exit: DMA memory reset\n");

	// Exit Device Module
	platform_driver_unregister(&vga_dma_driver);
	cdev_del(my_cdev);
	device_destroy(my_class, MKDEV(MAJOR(my_dev_id),0));
	class_destroy(my_class);
	unregister_chrdev_region(my_dev_id, 1);
	dma_free_coherent(NULL, MAX_PKT_LEN, tx_vir_buffer, tx_phy_buffer);
	printk(KERN_INFO "vga_dma_exit: Exit device module finished\"%s\".\n", DEVICE_NAME);
}

module_init(vga_dma_init);
module_exit(vga_dma_exit);

